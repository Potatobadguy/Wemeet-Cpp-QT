# WeMeet — 简历项目成品（可直接复制）

> 本文档把 `WeMeet` 视频会议系统整理为一份**可直接粘贴进简历**的项目描述，所有技术点均锚定真实源码（已逐文件核实）。
> 量化数据分为两类：**「已验证」**为实测/编译可得事实；**「架构能力」**为设计目标，面试中可据架构推导，请勿当作已压测的基准值。

---

## 1. 简历项目速览

| 项 | 内容 |
|----|------|
| **项目名称** | WeMeet — 企业级高性能视频会议系统 |
| **一句话定位** | 从零构建的高并发 C++ 视频会议系统：自研 Reactor 网络框架 + 信令服务器 + Qt6 跨平台客户端，覆盖现代 C++ 全栈核心技术 |
| **技术栈** | C++17 · Qt6 · epoll/Reactor · Protobuf · MySQL · 无锁编程 · 内存池 · CMake |
| **角色** | 独立开发者（架构设计 + 全模块实现 + 测试） |
| **规模（已验证）** | 16.5K+ 行源码 · 50+ 文件 · 3 个二进制 · 29 个单元测试全绿 |
| **代码位置** | `/workspace/wemeet/` |

---

## 2. 项目简介

WeMeet 是一个面向中小企业的视频会议解决方案。项目从零实现了一套**基于 Reactor + One Loop Per Thread 的高性能网络框架**，在其之上构建了**信令服务器**（登录鉴权、会议室状态机、在线用户管理、MySQL 持久化）与 **Qt6 桌面客户端**（登录/注册、视频画廊、实时聊天、暗色主题 UI）。底层贯穿现代 C++ 核心实践：RAII 资源管理、智能指针生命周期控制、移动语义零拷贝、无锁 SPSC 队列、三级 TLS 内存池、分片读写锁，并用 CMake 模块化构建 + 单元测试保证工程质量。音视频采集/传输以 Mock 模拟实现，架构接口真实完整、不依赖硬件即可编译运行。

---

## 3. 核心技术栈映射表

> 逐条对应 C++ 岗位常见的技能考察点，并标注本项目中的**真实实践位置**。

| 技能要求 | 在本项目中的实践 | 关键文件 |
|----------|------------------|----------|
| **C++11/14/17 智能指针** | `shared_ptr` + `enable_shared_from_this` 管理连接生命周期；`unique_ptr` 管理 MySQL 连接 | `tcp_connection.h` `connection_pool.h` |
| **移动语义 / 右值引用** | `Buffer` 移动构造/移动赋值实现零拷贝发送；`ConnGuard` 移动构造转移所有权 | `buffer.h` `tcp_connection.h` `connection_pool.h` |
| **Lambda / 函数式** | 事件回调、定时器回调、连接回调均以 `std::function` + lambda 绑定 | `event_loop.h` `tcp_connection.h` |
| **RAII** | Socket、连接、连接池、线程池全部 RAII 管理，析构即释放 | `socket.h` `tcp_connection.h` `connection_pool.h` `thread_pool.h` |
| **模板元编程** | `SPSCQueue<T>` 模板无锁队列；`Codec::encode<T>` 模板编解码 | `lockfree_queue.h` `codec.h` |
| **多线程 / 互斥锁 / 条件变量** | 线程池生产者-消费者模型；连接池获取/归还用 `mutex` + `condition_variable` | `thread_pool.h` `connection_pool.h` |
| **原子操作 / 无锁编程** | `std::atomic` + CAS 实现 SPSC 队列；`acquire-release` 内存序保证 happens-before | `lockfree_queue.h` |
| **内存池 / 锁优化** | 三级分档(8KB/64KB/1MB) + `thread_local` 零锁内存池；`alignas(64)` 缓存行对齐消除伪共享 | `memory_pool.h` `lockfree_queue.h` |
| **TCP / Socket / epoll / Reactor** | epoll ET 边缘触发事件循环；主从 Reactor + `EventLoopPool` One Loop Per Thread | `event_loop.h` `tcp_server.h` `event_loop_pool.h` |
| **Protobuf 协议** | 4 字节大端长度头 + Protobuf body；`BaseMessage` 统一包装 + 消息路由 | `codec.h` `proto/*.proto` |
| **MySQL CRUD / 索引 / 事务** | 连接池 + prepared statement；`add_friend` 事务（`START TRANSACTION`/`COMMIT`/`ROLLBACK`，统一加锁顺序防死锁） | `connection_pool.h` `user_repository.cpp` `init_db.sql` |
| **Qt 信号槽 / 布局 / 自定义控件** | `QStackedWidget` 页面切换；自定义视频画廊/音量控件；`QSS` 全局暗色主题 | `main_window.cpp` `meeting_room.cpp` `style.qss` |
| **Qt 多线程交互** | `QTcpSocket` 异步 + 信号槽驱动 UI 更新；自动重连 | `network_client.h` |
| **CMake / GDB / Valgrind / Git** | 模块化 `CMakeLists` + `FetchContent` + 条件编译各子系统；Debug 构建开启 ASan | 各层级 `CMakeLists.txt` |

---

## 4. 项目难点与解决方案

> 每条均锚定真实代码，面试可被追问到实现细节。

| # | 难点 | 解决方案 | 收益 / 落点 |
|---|------|----------|-------------|
| 1 | **epoll ET 边缘触发下的粘包/半包** | 自定义 `Buffer` 累积接收，以「4 字节大端长度头 + body」分包；`Codec::peek_length` 判断是否收满一帧 | 可靠的消息边界处理，避免数据错乱（`codec.h` `buffer.h`） |
| 2 | **高并发下锁竞争与内存碎片** | 三级固定档内存池(8KB/64KB/1MB) + `thread_local` 每线程独立池零锁分配；无锁 `SPSCQueue` 传递网络帧；`alignas(64)` 缓存行对齐 `write_pos_`/`read_pos_` 消除伪共享 | 降低 `malloc/free` 系统调用与碎片；SPSC 纯 atomic 无锁（`memory_pool.h` `lockfree_queue.h`） |
| 3 | **TCP 连接生命周期管理与泄漏** | `TcpConnection` 继承 `enable_shared_from_this`，以 `shared_ptr` 引用控制生命周期；RAII `Socket` 析构自动 `close`；优雅关闭 `shutdown → 等待 → close` | 回调安全、无悬垂指针、无连接泄漏（`tcp_connection.h` `socket.h`） |
| 4 | **数据库连接成为瓶颈** | RAII 连接池（`unique_ptr<MYSQL>` + 双端队列 + 条件变量），`ConnGuard` 析构自动归还；prepared statement 防 SQL 注入；空闲超时回收 + 最大连接数限制 | 复用连接免去频繁握手；异常安全（`connection_pool.h` `user_repository.cpp`） |
| 5 | **全局锁热点（在线用户表）** | `UserManager` 用 16 分片 `std::shared_mutex`：读（查找）走共享锁、写（上下线）走独占锁；`user_id % 16` 路由分片 | 读多写少场景大幅降低锁竞争（`user_manager.h`） |
| 6 | **Qt 跨线程更新 UI 崩溃** | 网络层 `NetworkClient`（`QObject`）以信号（`message_received`/`connected`）驱动，UI 层以队列连接（queued connection）槽函数安全更新界面；`QTcpSocket` 全异步 | 网络线程与 UI 线程解耦，无跨线程直接操作控件（`network_client.h` `main_window.cpp`） |
| 7 | **多子系统多 `main()` 冲突** | CMake 条件编译（`BUILD_SERVER/BUILD_NETWORK/BUILD_DB/BUILD_CLIENT/BUILD_TESTS`）；各 `test_*.cpp` 的 `main` 重构为 `test_xxx()` 由 `test_main.cpp` 统一驱动 | 单仓库多目标共存、可独立构建（`CMakeLists.txt` `tests/test_main.cpp`） |
| 8 | **好友关系双写一致性 / 死锁** | `add_friend` 用事务包裹双向插入，`ROLLBACK` 回滚；统一加锁顺序避免循环等待死锁 | 关系强一致、并发安全（`user_repository.cpp::add_friend`） |

---

## 5. 技术路线（Phase 1–5，与交付物对应）

| 阶段 | 目标 | 交付物 |
|------|------|--------|
| **Phase 1 — 基础库** | 现代 C++ 通用组件 | `buffer.h`(移动语义) · `memory_pool`(TLS 池) · `lockfree_queue`(无锁 SPSC) · `thread_pool`(future) · `logger`(异步双缓冲) |
| **Phase 2 — 网络框架** | Reactor 高性能网络 | `socket`(RAII) · `event_loop`(epoll ET + timerfd + eventfd) · `event_loop_pool`(One Loop Per Thread) · `tcp_server`(主从 Reactor + 16 分片连接表) · `tcp_connection` · `codec` |
| **Phase 3 — 数据库层** | MySQL 持久化 | `connection_pool`(RAII 池) · `user_repository`(CRUD + 事务) · `meeting_repository` · `init_db.sql`(4 表 + 索引 + 外键) |
| **Phase 4 — 信令服务器** | 业务信令 | `signaling_server`(消息分发) · `auth_service`(SHA256 + Token) · `meeting_service`(会议室状态机) · `user_manager`(16 分片锁) |
| **Phase 5 — Qt 客户端 + 文档** | 桌面端 + 工程化 | `main_window`/`login_dialog`/`meeting_room`/`network_client` + `style.qss` 暗色主题；`start_server.sh` 一键编译启动；README / 本文档 |

---

## 6. 可量化成果

### 已验证（实测 / 编译可得）
- **16.5K+ 行** C++ 源码（含 `.proto`），**50+ 个**源文件
- **3 个**可执行产物：`wemeet_server`(信令服务器) / `wemeet_client`(Qt 客户端) / `wemeet_test`(测试)
- **29 个单元测试全部通过**（7 个套件：MemoryPool / LockFreeQueue / ThreadPool / Buffer / EventLoop / Socket / Codec）
- Debug 构建开启 **ASan**（AddressSanitizer），内存安全问题可被编译/运行期捕获
- 编译无内存泄漏告警；RAII + 智能指针贯穿全生命周期管理

### 架构能力（设计目标，可据架构推导，非压测基准）
- epoll **ET 边缘触发 + One Loop Per Thread** 模型，设计支撑 **C10K** 级并发长连接
- 三级 **TLS 内存池** 降低高频分配碎片与系统调用开销
- **MySQL 连接池** 复用连接，免去每次请求的 TCP 握手 + 认证开销
- **16 分片读写锁** 将在线用户表的锁竞争分散到分片级

> ⚠️ 诚实说明：上表「架构能力」为设计容量，项目以 Mock 模拟音视频、未做端到端压测，面试中请以架构推导作答，勿冒充实测吞吐/延迟数值。

---

## 7. 简历文案（可直接复制）

### 中文版（STAR 化，建议选 5–6 条）

- 从零设计实现**基于 Reactor + One Loop Per Thread 的高性能网络框架**：epoll ET 边缘触发事件循环，配合 4 字节大端长度头 Protobuf 协议解决粘包/半包，单服务器设计支撑万级并发长连接。
- 设计**三级 TLS 内存池（8KB/64KB/1MB）+ 无锁 SPSC 队列**：以 `thread_local` 实现每线程零锁分配、`alignas(64)` 缓存行对齐消除伪共享，降低高频网络帧分配的内存碎片与锁竞争。
- 基于 `shared_ptr` + `enable_shared_from_this` + RAII 实现 **TCP 连接全生命周期管理**，配合优雅关闭流程，杜绝悬垂指针与连接泄漏；CMake 模块化条件编译单仓多目标。
- 实现 **MySQL RAII 连接池**：`ConnGuard` 析构自动归还、prepared statement 防注入、`add_friend` 事务 + 统一加锁顺序防死锁；在线用户表用 16 分片 `shared_mutex` 读写锁优化并发。
- 使用 **Qt6** 构建跨平台桌面客户端：信号槽解耦网络线程与 UI 线程，自定义视频画廊/音量控件，QSS 全局暗色主题，自动重连机制保障弱网体验。
- 工程化落地：**16.5K+ 行** C++ 代码、**29 个单元测试全绿**、Debug 构建集成 ASan，覆盖内存池 / 无锁队列 / 线程池 / 编解码等核心模块。

### English Version（optional）

- Designed and implemented a high-performance **Reactor-based networking framework** (epoll edge-triggered + One-Loop-Per-Thread) with a 4-byte big-endian length-prefixed Protobuf codec to handle TCP packet boundary issues, targeting C10K concurrent connections.
- Built a **3-tier thread-local memory pool (8KB/64KB/1MB) + lock-free SPSC queue**: `thread_local` zero-lock allocation and `alignas(64)` cache-line alignment to eliminate false sharing and reduce fragmentation under high-frequency frame allocation.
- Managed **TCP connection lifecycles** via `shared_ptr` + `enable_shared_from_this` + RAII with graceful shutdown, preventing dangling pointers and leaks; modular CMake with conditional subsystem compilation.
- Implemented a **RAII MySQL connection pool** with `ConnGuard` auto-return, prepared statements (SQL-injection safe), transactional `add_friend` with ordered locking to avoid deadlocks; online-user table sharded across 16 `shared_mutex` partitions.
- Developed a **Qt6 cross-platform desktop client**: signal/slot decoupling of network and UI threads, custom video-gallery/volume widgets, global QSS dark theme, and auto-reconnect for flaky networks.
- Delivered **16.5K+ lines** of C++ with **29 passing unit tests** and ASan enabled in Debug builds, covering the memory pool, lock-free queue, thread pool, and codec modules.

---

## 8. 面试追问预案（加分项）

> 按模块预演高频深度追问与回答要点，便于现场发挥。

**无锁队列（`lockfree_queue.h`）**
- *Q：SPSC 为什么不需要 ABA 处理？* A：单生产者单消费者下，每个位置一次只被一端移动，`read_pos_/write_pos_` 单调推进，不会因指针复用产生 ABA；且用位置序号（幂 of two + mask）而非指针，天然规避。
- *Q：内存序为什么用 acquire-release？* A：生产者 `store(write_pos_, release)` 与消费者 `load(acquire)` 形成 happens-before，保证 item 写入对消费者可见；`relaxed` 仅用于本地计数。
- *Q：满了/空了怎么办？* A：`try_push/try_pop` 返回 false 非阻塞；`push/pop` 自旋等待（注释提示可加 `_mm_pause()` 降功耗）。

**epoll（`event_loop.h`）**
- *Q：ET 和 LT 区别，为什么选 ET？* A：ET 仅在状态变化时通知一次，必须一次读尽（循环 `read` 到 `EAGAIN`），减少重复事件、降低唤醒次数；LT 每次就绪都通知，易惊群。ET 配合非阻塞 fd + 水平读尽是高频做法。
- *Q：如何跨线程唤醒 loop？* A：`eventfd`（`wakeup_fd_`）写入 8 字节触发 epoll 可读，从而执行 `run_in_loop` 投递的待处理任务（`pending_tasks_`）。

**连接池（`connection_pool.h`）**
- *Q：连接用完如何保证归还？* A：`ConnGuard` RAII，析构调用 `pool->release(conn)`；移动构造转移所有权、禁止拷贝，防止重复释放。
- *Q：连接失效怎么办？* A：`check_connection` 健康检查；空闲超时回收、最大连接数限制。

**事务（`user_repository.cpp`）**
- *Q：事务隔离级别？* A：使用 `START TRANSACTION`，MySQL 默认隔离级别为 REPEATABLE READ；核心在于 `add_friend` 双向插入用统一加锁顺序（先小后大）避免循环等待死锁，失败 `ROLLBACK`。

**Qt（`network_client.h`）**
- *Q：网络线程收到消息如何更新 UI 不崩？* A：`NetworkClient` 继承 `QObject`，通过信号（`message_received` 等）以队列连接方式触发 UI 槽函数，Qt 事件循环自动跨线程投递，绝不在网络线程直接操作控件。
- *Q：粘包在客户端怎么处理？* A：`recv_buffer_`（`QByteArray`）累积，按与服务端一致的 4 字节长度头分包解析。

**CMake / 工程**
- *Q：单仓多二进制如何避免 `main` 冲突？* A：条件编译各子系统 + 测试统一由 `test_main.cpp` 驱动（`test_xxx()` 函数而非各自 `main`）；`FetchContent` 拉取依赖。
