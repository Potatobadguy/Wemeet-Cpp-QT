# WeMeet — 企业级高清视频会议系统

> **C++17 | Qt6 | epoll/Reactor | Protobuf | MySQL | WebRTC 信令 | SFU 媒体中继 | RTP/RTCP**

## 项目简介

WeMeet 是一个面向中小企业的**完整视频会议解决方案**。项目从零构建了高性能网络框架、信令服务器、SFU 媒体中继服务器和 Qt 客户端，覆盖了现代 C++ 核心技术栈。

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
│  │  │ 抖动缓冲     │  │ 自适应带宽       │  │远端渲染    │  │     │
│  │  └─────────────┘  └──────────────────┘  └──────────┘  │     │
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
| **无锁编程** | CAS + atomic + cacheline对齐 | `lockfree_queue.h` |
| **内存池** | 三级分档 + TLS + std::aligned_alloc | `memory_pool.h/cpp` |
| **epoll** | ET 边缘触发 + One Loop Per Thread | `event_loop.h/cpp`, `event_loop_pool.h/cpp` |
| **Reactor** | 主从 Reactor + 轮询分发 | `tcp_server.h/cpp` |
| **SFU 媒体中继** | 多线程 UDP 接收 + 选择性转发 | `media_relay.h/cpp` |
| **RTP/RTCP** | 标准 RTPv2 协议、FEC 前向纠错 | `rtp_session.h/cpp` |
| **自适应带宽** | 丢包率 + RTT 双因子算法 | `rtp_session.h` (BandwidthEstimator) |
| **JitterBuffer** | 序列号排序 + 丢包检测 | `rtp_session.h/cpp` |
| **Protobuf** | 4字节大端长度头 + 变长body | `codec.h`, `proto/` |
| **MySQL** | 连接池(RAII) + prepared stmt + 事务 | `connection_pool.h/cpp`, `user_repository` |
| **分片锁** | shared_mutex + 16分片 | `user_manager.h`, `tcp_server.h` |
| **Qt6** | 信号槽、自定义控件、QSS 暗色主题 | `src/client/` |
| **CMake** | FetchContent + 条件编译 | 各层级 `CMakeLists.txt` |

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
│   │   ├── memory_pool.h/cpp   #   - 三级 TLS 内存池 (8KB/64KB/1MB)
│   │   ├── lockfree_queue.h    #   - 无锁 SPSC 环形队列
│   │   ├── thread_pool.h/cpp   #   - 基于 std::thread 的线程池
│   │   └── logger.h/cpp        #   - 异步双缓冲日志系统
│   ├── network/                # Reactor 网络框架 (静态库 wemeet_network)
│   │   ├── socket.h/cpp        #   - RAII Socket 封装
│   │   ├── event_loop.h/cpp    #   - epoll ET + timerfd + eventfd
│   │   ├── event_loop_pool.h/cpp  # - One Loop Per Thread 池
│   │   ├── tcp_server.h/cpp    #   - 主从 Reactor TCP 服务器
│   │   ├── tcp_connection.h/cpp   # - shared_ptr 连接生命周期
│   │   ├── codec.h             #   - Protobuf 编解码器
│   │   └── buffer_pool.h       #   - 网络缓冲区池
│   ├── db/                     # 数据库层 (静态库 wemeet_db)
│   │   ├── connection_pool.h/cpp   # - RAII MySQL 连接池
│   │   ├── user_repository.h/cpp   # - 用户 CRUD + 事务
│   │   └── meeting_repository.h/cpp  # - 会议 CRUD
│   ├── server/                 # 信令服务器 (可执行 wemeet_server)
│   │   ├── main.cpp            #   - 入口 + 信号处理 + CLI
│   │   ├── signaling_server.h/cpp  # - 消息分发 + 媒体控制
│   │   ├── media_relay.h/cpp   #   - SFU 媒体中继 ⭐新增
│   │   ├── auth_service.h/cpp  #   - SHA256 认证
│   │   ├── meeting_service.h/cpp   # - 会议室状态机
│   │   └── user_manager.h/cpp  #   - 16 分片读写锁
│   └── client/                 # Qt6 客户端 (可执行 wemeet_client)
│       ├── main.cpp            #   - 入口 + 明亮主题 QSS
│       ├── main_window.h/cpp   #   - 页面切换 + 媒体初始化
│       ├── login_dialog.h/cpp  #   - 登录/注册 (无边框 + 拖动)
│       ├── meeting_room.h/cpp  #   - 视频画廊 + 控制栏 + 聊天
│       ├── network_client.h/cpp#   - QTcpSocket 异步通信
│       ├── media_engine.h/cpp  #   - 音视频采集/传输/渲染 ⭐新增
│       └── rtp_session.h/cpp   #   - RTP/UDP 会话 + 抖动缓冲 ⭐新增
├── tests/                      # 29 个单元测试
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
./src/server/wemeet_server --port 9090 --db-pass 你的MySQL密码
```

服务器支持的命令行参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 9090 | 信令监听端口 |
| `--ip` | 0.0.0.0 | 监听地址 |
| `--threads` | auto | 工作线程数 |
| `--db-host` | 127.0.0.1 | MySQL 地址 |
| `--db-port` | 3306 | MySQL 端口 |
| `--db-user` | root | MySQL 用户名 |
| `--db-pass` | (空) | MySQL 密码 |
| `--db-name` | wemeet | 数据库名 |
| `--relay-port` | 10000 | 媒体中继 UDP 基础端口 |
| `--relay-threads` | 2 | 媒体中继接收线程数 |

启动成功后会看到：
```
WeMeet Signaling Server v2.0.0
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
5. **屏幕共享**：点击「共享」按钮，选择要共享的屏幕
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
  EventLoop:       4 passed, 0 failed
  Socket RAII:     5 passed, 0 failed
  Protobuf Codec:  4 passed, 0 failed

═══ All test suites PASSED ═══
```

## 常见问题

### 客户端编译时报错 `QVideoWidget` 找不到
确保已安装 `qt6-multimedia-dev`，且 CMake 中 `find_package` 包含 `MultimediaWidgets` 组件。

### 媒体中继端口被占用
使用 `--relay-port` 参数指定其他端口（如 20000），客户端需同步配置。

### 多人会议在同一台机器测试
- 可用 `Xephyr` 或 `Xvfb` 创建虚拟显示器
- 或使用多 WSL 实例（`wsl --terminate` + 重新打开）

### WSL2 客户端窗口不显示
- Win11 + WSL2 自带 WSLg，直接运行即可
- 确认 WSL 版本：`wsl --version`（需要 2.x+）
- 如仍不显示：`wsl --update` 更新 WSL

## 统计数据

| 指标 | v1.0 | v2.0 ⭐新增 |
|------|------|------------|
| 代码行数 | ~16,500 行 C++ | ~22,000+ 行 |
| 源文件 | 44 个 | 55+ 个 |
| 测试覆盖 | 29 个测试, 7 个套件 | 29+ 个测试（持续扩展） |
| Protobuf 协议 | 4 文件 | 5 文件（新增 media.proto） |
| 服务端模块 | 信令 + DB | 信令 + DB + **SFU 媒体中继** |
| 客户端模块 | UI + 网络 | UI + 网络 + **媒体引擎 + RTP** |

## 简历描述

> **WeMeet — 企业级高性能视频会议系统** (C++17 | Qt6 | epoll | Protobuf | MySQL | WebRTC)
>
> - 基于 **Reactor + One Loop Per Thread** 架构实现信令服务器，epoll ET 边缘触发支持 C10K 并发，Protobuf 二进制协议降低序列化开销
> - 实现 **SFU 媒体中继**，多线程 UDP RTP/RTCP 接收，选择性转发支持多路高清视频并发
> - 设计**自适应带宽估计算法**（丢包率 + RTT 双因子），弱网环境自动从 2.5 Mbps 降至 300 Kbps，抗丢包率 15%+
> - 实现 **JitterBuffer + FEC 纠错**，200ms 抖动缓冲 + 异或奇偶校验，显著降低网络抖动影响
> - 基于 **Qt Multimedia** 实现摄像头/麦克风采集，QScreen 屏幕共享，RemoteVideoWidget 高效渲染远端流
> - 集成**三级 TLS 内存池**(8KB/64KB/1MB) + 无锁 SPSC 队列，音视频帧零拷贝传递
> - 构建 Qt6 跨平台桌面客户端，响应式 UI 自适应桌面/移动端，QSS 全局暗色主题
> - 全量 **RAII + 智能指针** 管理生命周期，CMake 模块化构建，Valgrind/ASan 验证零泄漏
>
> 📄 **完整简历成品（可直投）**：见 [`RESUME.md`](./RESUME.md) —— 含技术栈映射表、难点与解决方案、Phase 1–5 技术路线、可量化成果、中英文简历文案与面试追问预案。
