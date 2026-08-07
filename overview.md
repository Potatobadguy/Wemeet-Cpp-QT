# WeMeet 优化点分析报告

> 基于全量源码审查，覆盖 common / network / db / server / client 五大模块共 50+ 文件

## TL;DR

项目架构扎实（Reactor + SFU + 自适应带宽），但在**媒体中继热路径**上存在一个 P0 级死锁 bug 和多个 O(N) 性能瓶颈。修复这些问题可以让 SFU 转发性能从"每包 O(房间×参与者×流)"降到 O(1)。

---

## P0 — 致命缺陷（必须立即修复）

### 1. `report_stats` → `suggest_bitrate` 递归死锁

**文件**: `src/server/media_relay.cpp` L428-451, L474-495

`report_stats()` 持有 `rooms_mutex_` 后调用 `suggest_bitrate()`，后者再次尝试 `lock(rooms_mutex_)`。`std::mutex` 不可重入 → **死锁**。

```cpp
// report_stats (L432)
std::lock_guard<std::mutex> lock(rooms_mutex_);     // ← 已锁
    ...
    send_bandwidth_hint(pit->second, suggest_bitrate(user_id, room_id));
//                                              ↑
// suggest_bitrate (L475)                            |
    std::lock_guard<std::mutex> lock(rooms_mutex_); // ← 死锁！
```

**修复方案**: 将 `suggest_bitrate` 拆为内部无锁版本 `_suggest_bitrate_unlocked()`，public 版本加锁后调用内部版本。或在 `report_stats` 中先释放锁再调用。

---

### 2. `inet_ntoa` 非线程安全

**文件**: `src/server/media_relay.cpp` L181, L264

`inet_ntoa` 返回指向静态缓冲区的指针，多线程调用会产生数据竞争。当前 relay 有多个工作线程并发调用。

**修复**: 替换为 `inet_ntop(AF_INET, &addr.sin_addr, buf, INET_ADDRSTRLEN)`。

---

## P1 — 性能瓶颈（热路径优化）

### 3. SSRC 反查 O(N) 全量扫描 — 每个媒体包都执行

**文件**: `src/server/media_relay.cpp` L192-215

每个 RTP 包到达时，通过**三层嵌套循环**（rooms → participants → streams）遍历查找匹配的 SSRC。在 10 人会议中，每包需要扫描 10×3=30 次。30fps 视频每秒 300+ 包，每路流每秒 30×30=900 次扫描。

**修复**: 维护 `std::unordered_map<uint32_t /*ssrc*/, SsrcRoutingInfo>` 索引，register/unregister 时更新，查询降为 O(1)。

### 4. `get_room_participants` 死拷贝 — 每个转发包都执行

**文件**: `src/server/media_relay.cpp` L219

```cpp
auto room = get_room_participants(found_room);  // ← 深拷贝所有参与者，返回值从未使用
```

这行代码在**每个 RTP 转发包**上复制整个房间参与者列表（含 string、map、shared_ptr），然后完全不用。纯浪费 CPU 和内存带宽。

**修复**: 直接删除这行代码。

### 5. 转发热路径中 `inet_pton` 逐包地址解析

**文件**: `src/server/media_relay.cpp` L235

```cpp
inet_pton(AF_INET, si->client_host.c_str(), &dest.sin_addr);  // 每个包每个目标都执行
```

`client_host` 是 string，每次转发都要做字符串→二进制地址转换。

**修复**: 在 `RTPStreamInfo` 中预存 `struct sockaddr_in dest_addr`，register 时一次性解析。

### 6. EventLoop 事件分发加锁

**文件**: `src/network/event_loop.cpp` L89-109

每个 EPOLLIN/EPOLLOUT 事件都通过 `fd_callbacks_mutex_` 加锁查找回调。但 EventLoop 是单线程的（One Loop Per Thread），回调表本不需要锁。锁仅用于跨线程注册 fd 时。

**修复**: fd 回调注册通过 `run_in_loop()` 投递到 IO 线程执行，消除热路径锁。或使用无锁结构。

### 7. SPSCQueue 自旋无退避

**文件**: `src/common/lockfree_queue.h` L48-53, L71-75

`push()`/`pop()` 的自旋循环没有 `_mm_pause()`，在 x86 上会导��流水线停顿和功耗浪费。注释提到了但未实现。

**修复**:
```cpp
while (!try_push(item)) {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();  // 或 __builtin_ia32_pause()
#endif
}
```

---

## P2 — 代码质量 / 设计缺陷

### 8. `cancel_timer` 空实现 — 资源泄漏

**文件**: `src/network/event_loop.cpp` L212-214

```cpp
void EventLoop::cancel_timer(int /*timer_id*/) {
    // 简化实现：暂不按 id 取消
}
```

`run_after`/`run_every` 返回的 timer_id 无法用于取消。一次性定时器如果回调未触发，timerfd 和 TimerEntry 会泄漏。

### 9. RTPHeader 重复定义

**文件**: `src/server/media_relay.h` L40-64 + `src/client/rtp_session.h` L27-34

两处独立定义了 `RTPHeader`，字段相同但方法不同（server 版有 helper 方法，client 版没有）。维护时容易产生不一致。

**修复**: 抽取到 `src/common/rtp_header.h` 共享。

### 10. ConnectionPool `idle_count()` 数据竞争

**文件**: `src/db/connection_pool.h` L62

```cpp
size_t idle_count() const { return idle_conns_.size(); }  // 无锁访问
```

`idle_conns_` 被 `acquire()`/`release()` 在锁保护下修改，但 `idle_count()` 不加锁直接读 → UB。

**修复**: 加锁或用原子计数器。

### 11. RTCPReportBlock 位域 — 网络协议解析用位域不可移植

**文件**: `src/server/media_relay.h` L73-83

```cpp
uint32_t fraction_lost : 8;
uint32_t cumulative_lost : 24;
```

位域的内存布局是实现定义的，不同编译器/平台可能不同。实际解析代码（L571-579）用的是手动字节提取，这个 struct 是死代码。

**修复**: 删除未使用的 `RTCPReportBlock` struct，或改为手动字节解析的辅助函数。

### 12. SSRC 顺序分配 — 可预测且可能碰撞

**文件**: `src/server/media_relay.cpp` L528-530

```cpp
uint32_t MediaRelay::allocate_ssrc() {
    return next_ssrc_++;  // 从 1000 开始递增
}
```

SSRC 应该是随机数（RFC 3550 建议），顺序分配可被预测/伪造。

**修复**: 使用 `std::random_device` 或基于 user_id + timestamp 的哈希。

### 13. `relay_sockets_` 在 stop() 中无锁访问

**文件**: `src/server/media_relay.cpp` L77-79

`relay_sockets_` 在 `relay_thread_func` 中通过 `rooms_mutex_` 保护写入，但在 `stop()` 中无锁遍历和 close。

### 14. 默认密码硬编码

**文件**: `src/server/signaling_server.h` L36

```cpp
std::string db_pass = "123456";  // Config 默认值
```

数据库密码不应有非空默认值。

---

## P3 — 工程实践改进

### 15. MemoryPool 用 `vector<void*>` 管理空闲链表

内存池自身的管理结构用 `std::vector` 分配内存，略显讽刺。可改为侵入式链表（free block 自身存 next 指针）。

### 16. Logger 变参格式化 — 无编译期检查

`printf` 风格的变参无法在编译期检查格式串与参数类型匹配。可考虑 `std::format` (C++20) 或 `folly::format` 风格。同时 `log()` 在锁内做 `vsnprintf` 格式化，应先格式化到栈缓冲再拷贝到双缓冲。

### 17. 缺少 RTP 包校验

MediaRelay 对传入 RTP 包仅检查最小长度（12 字节），不校验 version、CSRC count、padding 等。恶意客户端可发送畸形包。

### 18. Relay 线程函数中的 static 局部变量

**文件**: `src/server/media_relay.cpp` L249, L260

```cpp
static std::atomic<uint64_t> fwd_seq{0};
static std::atomic<uint64_t> unknown_seq{0};
```

函数局部 static 在多线程环境下共享，应移为类成员，语义更清晰。

---

## 优化优先级总览

| 优先级 | 问题数 | 预期收益 |
|--------|--------|----------|
| **P0 致命** | 2 | 修复死锁 + 线程安全 |
| **P1 性能** | 5 | SFU 转发路径 O(N)→O(1)，吞吐提升数量级 |
| **P2 质量** | 7 | 消除 UB、数据竞争、维护隐患 |
| **P3 工程** | 4 | 长期可维护性提升 |

**建议修复顺序**: P0 → P1.3(SSRC索引) → P1.4(死拷贝) → P1.5(地址缓存) → 其余
