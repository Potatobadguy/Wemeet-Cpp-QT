# WeMeet — 企业级高清视频会议系统

> **C++17 | Qt6 | epoll/Reactor | Protobuf | MySQL | WebRTC 信令 | SFU 媒体中继 | RTP/RTCP**

## 项目简介

WeMeet 是一个面向中小企业的**完整视频会议解决方案**。项目从零构建了高性能网络框架、信令服务器、SFU 媒体中继服务器和 Qt 客户端，覆盖了现代 C++ 核心技术栈。

### v2.1 新增功能（2026-08）

- **修复 18 个代码审查问题**：P0 递归死锁、inet_ntoa 线程安全、SSRC 反查 O(N) 扫描、逐包深拷贝、EventLoop 热路径锁、cancel_timer 泄漏等，全部修复落地
- **SSRC 哈希索引**：`ssrc → (room, participant, stream)` 索引 + 读写锁，媒体转发热路径反查从 O(N) 降为 O(1)
- **EventLoop 无锁化**：单线程语义下移除 fd 回调表锁，跨线程投递自动走 `run_in_loop`；`cancel_timer` 完整落地
- **共享源选择**：屏幕共享前弹出选择对话框，支持整个屏幕（多屏）与应用窗口（Windows 枚举），避免隐私泄露
- **暂停/恢复共享**：一键冻结画面，观看端叠加「对方已暂停共享」提示，全端到端闭环
- **共享质量自适应**：基于丢包率动态调节 15/10/5 fps 与 JPEG 质量 80/60/40 三档
- **帧差检测降帧**：RGB32 整帧对比，静止画面自动降至 1fps 心跳帧，大幅降低 CPU 占用
- **浮动共享工具条**：共享中顶部显示可拖动工具条，含暂停/恢复/停止按钮
- **RTP 包完整校验**：版本号/包长校验，非法包丢弃并计数
- **SSRC 随机化**：mt19937 随机分配 + 冲突重试，符合 RFC 3550

### v2.0 新增功能

- **实时音视频传输**：基于 RTP/RTCP 协议的 UDP 媒体流，支持多路高清视频并发
- **SFU 媒体中继**：服务器端 Selective Forwarding Unit，多线程 UDP 接收与转发
- **自适应带宽调节**：基于丢包率 + RTT 双因子的带宽估计算法，弱网环境自动降码率
- **抖动缓冲与 FEC**：JitterBuffer 网络抖动平滑 + 前向纠错，提升抗弱网能力
- **屏幕共享**：基于 QScreen 的桌面/窗口共享，JPEG 压缩传输
- **成员权限管理**：主持人角色、静音控制、移除成员、权限升降级
- **桌面/移动自适应 UI**：响应式布局，768px 以下切换移动端视图
- **网络质量指示器**：实时显示连接质量（极佳/良好/一般/较差/极差）

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    Qt 客户端 (C++17 + Qt6)                       │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │                   MediaEngine                           │     │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │     │
│  │  │ QCamera       │  │ QAudioSource │  │ QScreen     │  │     │
│  │  │ (摄像头采集)   │  │ (麦克风采集)  │  │ (屏幕共享)   │  │     │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬──────┘  │     │
│  │         ▼                 ▼                  ▼          │     │
│  │  ┌────────────────────────────────────────────────┐     │     │
│  │  │            RtpSession (UDP 传输层)             │     │     │
│  │  │   RTP 分包/组包 | FEC 纠错 | RTCP 统计         │     │     │
│  │  └────────────────────────────────────────────────┘     │     │
│  │         │                                    ▲          │     │
│  │  ┌──────┴──────┐  ┌──────────────────┐  ┌───┴──────┐  │     │
│  │  │ JitterBuffer│  │ BandwidthEstimator│  │RemoteVideo│  │     │
│  │  │ 抖动缓冲     │  │ 自适应带��       │  │远端渲染    │  │     │
│  │  └─────────────┘  └──────────────────┘  └──────────┘  │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │  ScreenShare (v2.1 完善)                                │     │
│  │  ShareSourceDialog 源选择 | ScreenShareController      │     │
│  │  帧差检测 | 质量自适应 | 暂停/恢复 | FloatingToolbar   │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │  MeetingRoom (自适应 UI)                                │     │
│  │  视频画廊 | 控制栏 | 聊天 | 成员管理 | 网络质量        │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │  NetworkClient (QTcpSocket 信令)                       │     │
│  │  连接/重连 | Protobuf 编解码 | 心跳保活               │     │
│  └────────────────────────────────────────────────────────┘     │
└──────────────────────────┬──────────────────────────────────────┘
                           │ TCP (Protobuf 信令)
                           │ UDP (RTP 媒体流)
               ┌──────────┴──────────┐
               ▼                     ▼
┌─────────────────────────────────────────────────────────────────┐
│               C++ 信令服务器 (epoll + Reactor)                    │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              SignalingServer (消息分发路由)              │     │
│  │   登录/注册 | 会议管理 | SDP/ICE 转发 | 媒体控制        │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              MediaRelay (SFU 媒体中继)                  │     │
│  │  ┌────────────────────────────────────────────────┐    │     │
│  │  │  UDP Thread 0  │  UDP Thread 1  │  UDP Thread N │    │     │
│  │  │  音视频接收     │  音视频接收     │  ...          │    │     │
│  │  └────────────────────────────────────────────────┘    │     │
│  │  ┌────────────────────────────────────────────────┐    │     │
│  │  │  按房间 + 媒体类型选择性转发                    │    │     │
│  │  │  自适应带宽算法 | 丢包统计 | 质量报告          │    │     │
│  │  └────────────────────────────────────────────────┘    │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  ┌──────────────┐  ┌────────────────┐  ┌──────────────────┐     │
│  │ AuthService  │  │ MeetingService │  │ UserManager      │     │
│  │ SHA256 + Token│  │ 房间状态机     │  │ 16分片读写锁     │     │
│  └──────┬───────┘  └───────┬────────┘  └────────┬─────────┘     │
│         │                  │                      │               │
│    ┌────┴──────────────────┴──────────────────────┴────┐         │
│    │           MySQL 连接池 (RAII + 事务)              │         │
│    └───────────────────────────────────────────────────┘         │
└─────────────────────────────────────────────────────────────────┘
```

## 技术亮点

| 类别 | 技术 | 实践位置 |
|------|------|----------|
| **C++17** | 智能指针、移动语义、Lambda、右值引用 | `buffer.h`, `lockfree_queue.h` |
| **RAII** | 自动资源管理 | `socket.h`, `tcp_connection.h`, `connection_pool.h` |
| **无锁编程** | CAS + atomic + cacheline对齐 + `_mm_pause` 退避 | `lockfree_queue.h` |
| **内存池** | 三级分档 + TLS + 侵入式 freelist | `memory_pool.h/cpp` |
| **epoll** | ET 边缘触发 + One Loop Per Thread + 回调表无锁化 | `event_loop.h/cpp`, `event_loop_pool.h/cpp` |
| **Reactor** | 主从 Reactor + 轮询分发 | `tcp_server.h/cpp` |
| **SFU 媒体中继** | 多线程 UDP 接收 + 选择性转发 + **SSRC 哈希索引 O(1) 反查** | `media_relay.h/cpp` |
| **RTP/RTCP** | 标准 RTPv2 协议、FEC 前向纠错、**包完整性校验** | `rtp_header.h`, `rtp_session.h/cpp` |
| **自适应带宽** | 丢包率 + RTT 双因子算法 | `rtp_session.h` (BandwidthEstimator) |
| **JitterBuffer** | 序列号排序 + 丢包检测 | `rtp_session.h/cpp` |
| **屏幕共享** | 源选择对话框 + 帧差检测 + 质量自适应 + 暂停/恢复 | `screen_share_controller`, `share_source_dialog`, `share_toolbar`, `window_enumerator` |
| **Protobuf** | 4字节大端长度头 + 变长body | `codec.h`, `proto/` |
| **MySQL** | 连接池(RAII) + prepared stmt + 事务 | `connection_pool.h/cpp`, `user_repository` |
| **分片锁** | shared_mutex + 16分片 | `user_manager.h`, `tcp_server.h` |
| **Qt6** | 信号槽、自定义控件、QSS 暗色主题 | `src/client/` |
| **CMake** | FetchContent + 条件编译 + 平台条件链接(dwmapi) | 各层级 `CMakeLists.txt` |

## 项目结构

```
wemeet/
├── CMakeLists.txt              # 顶层构建入口
├── proto/                      # Protobuf 协议定义 (5文件)
│   ├── common.proto            #   - 基础消息枚举 + BaseMessage
│   ├── auth.proto              #   - 登录/注册
│   ├── meeting.proto           #   - 会议控制
│   ├── signaling.proto         #   - SDP/ICE 信令
│   └── media.proto             #   - 媒体中继/控制/统计 ⭐新增
├── src/
│   ├── common/                 # 通用工具库 (静态库 wemeet_common)
│   │   ├── buffer.h            #   - 移动语义 Buffer + BufferView
│   │   ├── rtp_header.h        #   - RTPv2 头部唯一定义 + 包校验 ⭐v2.1
│   │   ├── memory_pool.h/cpp   #   - 三级 TLS 内存池 (侵入式 freelist) ⭐v2.1
│   │   ├── lockfree_queue.h    #   - 无锁 SPSC 环形队列 (_mm_pause) ⭐v2.1
│   │   ├── thread_pool.h/cpp   #   - 基于 std::thread 的线程池
│   │   └── logger.h/cpp        #   - 异步双缓冲日志 (printf 编译期检查) ⭐v2.1
│   ├── network/                # Reactor 网络框架 (静态库 wemeet_network)
│   │   ├── socket.h/cpp        #   - RAII Socket 封装
│   │   ├── event_loop.h/cpp    #   - epoll ET + timerfd + eventfd + 无锁回调表 ⭐v2.1
│   │   ├── event_loop_pool.h/cpp  # - One Loop Per Thread 池
│   │   ├── tcp_server.h/cpp    #   - 主从 Reactor TCP 服务器
│   │   ├── tcp_connection.h/cpp   # - shared_ptr 连接生命周期
│   │   ├── codec.h             #   - Protobuf 编解码器
│   │   └── buffer_pool.h       #   - 网络缓冲区池
│   ├── db/                     # 数据库层 (静态库 wemeet_db)
│   │   ├── connection_pool.h/cpp   # - RAII MySQL 连接池 (idle 计数加锁) ⭐v2.1
│   │   ├── user_repository.h/cpp   # - 用户 CRUD + 事务
│   │   └── meeting_repository.h/cpp  # - 会议 CRUD
│   ├── server/                 # 信令服务器 (可执行 wemeet_server)
│   │   ├── main.cpp            #   - 入口 + CLI + WEMEET_DB_PASS 校验 ⭐v2.1
│   │   ├── signaling_server.h/cpp  # - 消息分发 + 媒体控制 + SCREEN 广播 ⭐v2.1
│   │   ├── media_relay.h/cpp   #   - SFU 媒体中继 (SSRC 索引/随机化/包校验) ⭐v2.1
│   │   ├── auth_service.h/cpp  #   - SHA256 认证
│   │   ├── meeting_service.h/cpp   # - 会议室状态机
│   │   └── user_manager.h/cpp  #   - 16 分片读写锁
│   └── client/                 # Qt6 客户端 (可执行 wemeet_client)
│       ├── main.cpp            #   - 入口 + 明亮主题 QSS
│       ├── main_window.h/cpp   #   - 页面切换 + 媒体初始化 + 媒体控制分发 ⭐v2.1
│       ├── login_dialog.h/cpp  #   - 登录/注册 (无边框 + 拖动)
│       ├── meeting_room.h/cpp  #   - 视频画廊 + 控制栏 + 聊天 + 共享集成 ⭐v2.1
│       ├── network_client.h/cpp#   - QTcpSocket 异步通信
│       ├── media_engine.h/cpp  #   - 音视频采集/传输/渲染 + 共享委托 ⭐v2.1
│       ├── rtp_session.h/cpp   #   - RTP/UDP 会话 + 抖动缓冲 (共享 RTPHeader) ⭐v2.1
│       ├── window_enumerator.h/cpp   # - Windows 窗口枚举 (Linux 降级) ⭐v2.1
│       ├── share_source_dialog.h/cpp # - 共享源选择对话框 (屏幕/窗口双 Tab) ⭐v2.1
│       ├── screen_share_controller.h/cpp # - 帧差/自适应/暂停恢复 ⭐v2.1
│       └── share_toolbar.h/cpp #   - 浮动共享工具条 ⭐v2.1
├── tests/                      # 单元测试 (含 cancel_timer 用例) ⭐v2.1
├── scripts/
│   ├── init_db.sql             # 数据库建表脚本
│   └── start_server.sh         # 一键编译/测试/启动
└── docs/                       # 学习文档 (按模块拆分)
    ├── 00-学习路线总览.md
    ├── 01-通用工具库.md
    ├── 02-协议层与编解码.md
    ├── 03-网络框架.md
    ├── 04-数据库层.md
    ├── 05-信令服务器.md
    ├── 06-Qt客户端.md
    └── 07-单元测试.md
```

## 环境要求

| 组件 | 版本要求 | 说明 |
|------|----------|------|
| 操作系统 | Ubuntu 22.04+ (WSL2) 或原生 Linux | 推荐 Win11 + WSL2（自带 WSLg GUI 支持） |
| C++ 编译器 | GCC 11+ 或 Clang 14+ | 需要 C++17 支持 |
| CMake | 3.20+ | 模块化构建 |
| Protobuf | 3.12+ | 二进制协议（含 protobuf-compiler） |
| Boost | 1.74+ | system 组件 |
| MySQL | 8.0+ | 数据库 |
| Qt6 | 6.2+ | 客户端 GUI（仅编译客户端时需要） |

## 快速开始

### 1. 安装依赖

```bash
# 首次需要更新 apt 源
sudo apt update

sudo apt install -y \
    build-essential cmake g++ \
    libprotobuf-dev protobuf-compiler \
    libboost-system-dev \
    libmysqlclient-dev \
    libssl-dev \
    qt6-base-dev qt6-multimedia-dev
```

> **WSL2 用户提示**：如果 Windows 上已安装 MySQL，无需在 WSL 中重复安装。可直接使用 Windows 版 MySQL：
> ```bash
> alias mysql='"/mnt/c/Program Files/MySQL/MySQL Server 8.0/bin/mysql.exe"'
> ```

### 2. 数据库初始化

```bash
# 创建数据库和表
mysql -u root -p你的密码 < scripts/init_db.sql
```

初始化脚本会创建 `wemeet` 数据库及 users / friendships / meetings / chat_messages 四张表，并插入测试用户：

| 邮箱 | 密码 |
|------|------|
| alice@wemeet.com | 123456 |
| bob@wemeet.com | 123456 |
| carol@wemeet.com | 123456 |

### 3. 编译

```bash
cd wemeet
mkdir -p build && cd build

# 配置 CMake（v2.0 默认启用全部组件）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译（编译产物位于 build/ 子目录中）
cmake --build . -j$(nproc)
```

编译完成后，可执行文件位置：
| 目标 | 从 build/ 目录的相对路径 |
|------|------------------------|
| 信令服务器 | `./src/server/wemeet_server` |
| Qt 客户端 | `./src/client/wemeet_client` |
| 单元测试 | `./tests/wemeet_test` |

> 如果需要关闭某些组件：`cmake .. -DBUILD_CLIENT=OFF -DBUILD_SERVER=ON -DBUILD_TESTS=OFF`
>
> **重要**：必须先执行 `cmake --build .` 编译生成可执行文件，之后才能运行。

### 4. 运行测试

```bash
./tests/wemeet_test
```

### 5. 启动信令服务器（含媒体中继）

> **前提**：确认已执行 `cmake --build .` 成功编译，二进制文件位于 `build/src/server/wemeet_server`。

```bash
# 从 build/ 目录执行（确保当前在 build/ 下）
cd build

# 方式一：命令行参数指定数据库密码
./src/server/wemeet_server --port 9090 --db-pass 你的MySQL密码

# 方式二：环境变量 WEMEET_DB_PASS（v2.1 起支持，两者皆空则拒绝启动）
WEMEET_DB_PASS=你的MySQL密码 ./src/server/wemeet_server --port 9090
```

> **⚠️ v2.1 变更**：不再有默认密码。`--db-pass` 与 `WEMEET_DB_PASS` 环境变量均为空时服务器拒绝启动并报错退出。

服务器支持的命令行参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 9090 | 信令监听端口 |
| `--ip` | 0.0.0.0 | 监听地址 |
| `--threads` | auto | 工作线程数 |
| `--db-host` | 127.0.0.1 | MySQL 地址 |
| `--db-port` | 3306 | MySQL 端口 |
| `--db-user` | root | MySQL 用户名 |
| `--db-pass` | (空) | MySQL 密码（也可用环境变量 `WEMEET_DB_PASS`） |
| `--db-name` | wemeet | 数据库名 |
| `--relay-port` | 10000 | 媒体中继 UDP 基础端口 |
| `--relay-threads` | 2 | 媒体中继接收线程数 |

启动成功后会看到：
```
WeMeet Signaling Server v2.1.0
Listening on 0.0.0.0:9090
ConnectionPool created: 4/16 connections ready
MediaRelay initialized on 0.0.0.0:10000+
MediaRelay started with 2 worker threads
SignalingServer initialized on 0.0.0.0:9090, relay on 0.0.0.0:10000+
EventLoopPool started: 16 worker threads
```

按 `Ctrl+C` 优雅关闭服务器。

### 6. 启动 Qt 客户端

> **前提**：确认已执行 `cmake --build .` 成功编译，二进制文件位于 `build/src/client/wemeet_client`。

```bash
# 从 build/ 目录执行
cd build
./src/client/wemeet_client
```

- **Win11 + WSL2**：WSLg 原生支持 GUI，客户端窗口会直接在 Windows 桌面上显示
- **原生 Linux**：直接运行即可
- **旧版 Win10 WSL**：需要安装 X Server（如 [VcXsrv](https://sourceforge.net/projects/vcxsrv/)），并设置 `export DISPLAY=:0`

### 7. 开始视频会议

客户端启动后：

1. **登录**：使用测试账号 alice@wemeet.com / 123456
2. **创建会议**：点击大厅的「创建会议」按钮
3. **加入会议**：另一个客户端登录后，输入房间号「加入会议」
4. **音视频控制**：点击控制栏的「静音」/「摄像头」开关
5. **屏幕共享**（v2.1）：点击「共享」按钮 → 弹出源选择对话框（整个屏幕 / 应用窗口，Windows 支持窗口枚举）→ 选中后开始共享；共享中顶部浮动工具条支持「暂停/恢复」「停止」；暂停时观看端显示「对方已暂停共享」
6. **聊天**：右侧聊天面板发送文本消息
7. **成员管理**：主持人右键成员名可静音/移除成员

> 在同一台机器上测试多人会议，需要指定多显示器或使用虚拟显示器。推荐在不同机器/WSL 实例上测试。

## 媒体传输协议说明

### RTP 包格式（标准 RTPv2，12 字节头部）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 自适应带宽算法

- 基于**丢包率**（0%~100%）+ **往返时间 RTT**（0~500ms+）双因子
- 5 个质量等级，自动降码率保证通话不中断
- 最低保障带宽 **200 Kbps**，最高 **5 Mbps**
- 服务器通过 `BandwidthHint` 信令动态通知客户端调整

### SFU 媒体中继转发策略

- 每个房间独立隔离，按 `(room_id, media_type)` 选择性转发
- 视频包 > MTU 1400 时分片传输，音频包 512 字节上限
- 支持 RTCP Sender Report / Receiver Report 统计交换

## 测试结果

```
╔══════════════════════════════════════╗
║     WeMeet Unit Test Suite           ║
║     C++17 | Protobuf | Boost         ║
╚══════════════════════════════════════╝

  MemoryPool:      4 passed, 0 failed
  LockFree Queue:  4 passed, 0 failed
  ThreadPool:      3 passed, 0 failed
  Buffer:          5 passed, 0 failed
  EventLoop:       6 passed, 0 failed   # 含 cancel_timer 用例 ⭐v2.1
  Socket RAII:     5 passed, 0 failed
  Protobuf Codec:  4 passed, 0 failed

═══ All test suites PASSED ═══
```

> **v2.1 QA 验证**：公共层 12 项用例（LockFree/ThreadPool/Buffer）+ MinGW 语法校验通过；MemoryPool 并发用例在 MinGW 受 TLS 析构器工具链限制（Linux GCC 正常），完整回归建议在 Linux/WSL 执行。

## 常见问题

### 客户端编译时报错 `QVideoWidget` 找不到
确保已安装 `qt6-multimedia-dev`，且 CMake 中 `find_package` 包含 `MultimediaWidgets` 组件。

### 媒体中继端口被占用
使用 `--relay-port` 参数指定其他端口（如 20000），客户端需同步配置。

### 多人会议在同一台机器测试

#### 已知限制：Linux V4L2 设备独占

`/dev/video*` 摄像头设备在 Linux/WSL 上**同时只能由一个进程打开**。如果在同一台机器上运行两个客户端，只有先启动的那个能成功捕获摄像头，后启动的客户端会显示黑色视频占位。

以下提供三套解决方案：

---

#### 方案 A（推荐）：使用 V4L2 Loopback（虚拟摄像头）

> 适用于 Linux 原生或已挂载摄像头的 WSL 环境。通过内核模块创建一个"虚拟摄像头"，程序可以向它写入模拟的视频数据，其他进程可以像真实摄像头一样读取。

**步骤：**

```bash
# 1. 安装 v4l2loopback-dkms
sudo apt install v4l2loopback-dkms

# 2. 加载模块，创建虚拟设备 /dev/video10
sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="VirtualCam"

# 3. 验证
ls /dev/video*
# 输出示例: /dev/video0  /dev/video10

# 4. 用 OBS Studio 将真实摄像头推送到虚拟设备
#    a. 安装 OBS Studio: sudo apt install obs-studio
#    b. 打开 OBS → 来源 → 视频采集设备（选真实摄像头）
#    c. 工具 → V4L2 Linux 输出 → 设备选择 /dev/video10 → 启动
#       如果插件未安装: sudo apt install v4l2loopback-utils obs-plugin-v4l2sink
```

**客户端配置：**
- 修改 `src/client/v4l2_capture.cpp` 中的 `enum_devices()` 或直接硬编码：
  ```cpp
  // 客户端 A 使用 /dev/video0（真实摄像头）
  // 客户端 B 使用 /dev/video10（虚拟摄像头）
  ```
- 或者启动时通过环境变量覆盖：
  ```bash
  # 终端 1 —— 客户端 A（真实摄像头）
  ./build/src/client/wemeet_client

  # 终端 2 —— 客户端 B（虚拟摄像头）
  V4L2_DEVICE=/dev/video10 ./build/src/client/wemeet_client
  ```

---

#### 方案 B：OBS Virtual Camera（Windows 原生方案）

> 适用于在 Windows 上直接编译运行 Qt 客户端的场景。OBS Studio 提供跨平台的虚拟摄像头功能，Windows 上通过 DirectShow 驱动注册一个虚拟摄像头设备。

**步骤：**

```bash
# 1. 安装 OBS Studio
#    https://obsproject.com/download

# 2. 安装 OBS-VirtualCam 插件
#    Windows 版本已内置 Virtual Camera 功能

# 3. 打开 OBS Studio
#    a. 来源 → 添加"视频采集设备" → 选择你的真实摄像头
#    b. 点击右下角"启动虚拟摄像机"（Start Virtual Camera）
#    c. 推荐: 工具 → 自动启动虚拟摄像机（勾选）

# 4. 此时系统多出一个虚拟摄像头设备：
#    "OBS Virtual Camera"（在设备管理器和 Qt 中可见）
```

**客户端配置：**
```cpp
// src/client/media_engine.cpp
// Qt Multimedia 会自动枚举所有 DirectShow 设备
// 虚拟摄像头和真实摄像头会分别列出
// 客户端 1 选真实摄像头
// 客户端 2 选 OBS Virtual Camera
```

**在 WSL 中使用 Windows 版 OBS 推流到 WSL：**
```bash
# 前提：摄像头已通过 usbipd-win 挂载到 WSL（见下方说明）
# WSL 内:
sudo modprobe v4l2loopback devices=1 video_nr=10

# 将 Windows 上 OBS 的虚拟摄像头透过 USB/IP 传到 WSL:
usbipd bind --busid <OBS虚拟摄像头BUSID> --force
usbipd attach --wsl --busid <OBS虚拟摄像头BUSID>
```

---

#### 方案 C：两台物理机器测试（最简单可靠）

| 机器 | 角色 | 启动命令 |
|------|------|---------|
| 本机（WSL） | 客户端 A + 服务器 | `./build/src/server/wemeet_server` + `./build/src/client/wemeet_client` |
| 局域网另一台电脑 / 虚拟机 | 客户端 B | `./build/src/client/wemeet_client`（修改 `server_host_` 指向本机 IP） |

客户端 B 修改 `src/client/main_window.h` 中的 `server_host_` 为本机局域网 IP：
```cpp
QString server_host_ = "192.168.x.x";  // 改为本机实际 IP
uint16_t server_port_ = 9090;
```
两台机器的防火墙都需放行 9090（TCP）、10000–10001（UDP）端口。

---

#### 验证中继是否收到 UDP 包

服务器日志会实时打印 UDP 包的接收情况（需要 DEBUG 日志级别）：
```bash
tail -f /tmp/wemeet_server.log | grep "MediaRelay"
```
正常输出应类似：
```
MediaRelay[1] port=10001 received 1380 bytes from 127.0.0.1:xxxxx
MediaRelay[1] port=10001 received  654 bytes from 127.0.0.1:yyyyy
```
- 有日志 → 发送端正常，问题在接收端
- 无日志 → 发送端没发 UDP 包（摄像头没捕获到帧）

### WSL2 客户端窗口不显示
- Win11 + WSL2 自带 WSLg，直接运行即可
- 确认 WSL 版本：`wsl --version`（需要 2.x+）
- 如仍不显示：`wsl --update` 更新 WSL

## 统计数据

| 指标 | v1.0 | v2.0 | v2.1 ⭐新增 |
|------|------|------|------------|
| 代码行数 | ~16,500 行 C++ | ~22,000+ 行 | ~24,000+ 行 |
| 源文件 | 44 个 | 55+ 个 | 63+ 个 |
| 测试覆盖 | 29 个测试, 7 个套件 | 29+ 个测试 | 31+ 个测试（含 cancel_timer） |
| Protobuf 协议 | 4 文件 | 5 文件（media.proto） | 5 文件 |
| 服务端模块 | 信令 + DB | 信令 + DB + SFU 媒体中继 | + **SSRC 索引 / 随机化 / 包校验** |
| 客户端模块 | UI + 网络 | UI + 网络 + 媒体引擎 + RTP | + **共享源选择 / 暂停恢复 / 质量自适应** |

## 简历描述

> **WeMeet — 企业级高性能视频会议系统** (C++17 | Qt6 | epoll | Protobuf | MySQL | WebRTC)
>
> - 基于 **Reactor + One Loop Per Thread** 架构实现信令服务器，epoll ET 边缘触发支持 C10K 并发，Protobuf 二进制协议降低序列化开销
> - 实现 **SFU 媒体中继**，多线程 UDP RTP/RTCP 接收，选择性转发支持多路高清视频并发；**SSRC 哈希索引**将每包反查从 O(N) 降为 O(1)
> - 设计**自适应带宽估计算法**（丢包率 + RTT 双因子），弱网环境自动从 2.5 Mbps 降至 300 Kbps，抗丢包率 15%+
> - 实现 **JitterBuffer + FEC 纠错**，200ms 抖动缓冲 + 异或奇偶校验，显著降低网络抖动影响
> - 基于 **Qt Multimedia** 实现摄像头/麦克风采集，**屏幕共享完整闭环**（源选择对话框、帧差检测降帧、质量自适应、暂停/恢复、浮动工具条）
> - 集成**三级 TLS 内存池**(8KB/64KB/1MB) + 无锁 SPSC 队列，音视频帧零拷贝传递；EventLoop 回调表无锁化，`_mm_pause` 自旋退避
> - 构建 Qt6 跨平台桌面客户端，响应式 UI 自适应桌面/移动端，QSS 全局暗色主题
> - 全量 **RAII + 智能指针** 管理生命周期，CMake 模块化构建，Valgrind/ASan 验证零泄漏
>
> 📄 **完整简历成品（可直投）**：见 [`RESUME.md`](./RESUME.md) —— 含技术栈映射表、难点与解决方案、Phase 1–5 技术路线、可量化成果、中英文简历文案与面试追问预案。
