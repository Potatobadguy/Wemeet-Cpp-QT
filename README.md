# WeMeet — 企业级高性能视频会议系统

> **C++17 | Qt6 | epoll/Reactor | Protobuf | MySQL | 无锁编程 | 内存池**

## 项目简介

WeMeet 是一个面向中小企业的视频会议解决方案。项目从零构建了高性能网络框架、信令服务器和 Qt 客户端，覆盖了现代 C++ 核心技术栈，适合作为简历中的 C++ 全栈项目。

## 系统架构

```
┌─────────────────────────────────────────────────┐
│              Qt 客户端 (C++17 + Qt6)             │
│  LoginDialog │ MeetingRoom │ NetworkClient       │
│  暗色主题 QSS │ 视频画廊 │ 聊天面板 │ 自定义控件  │
└────────────────────┬────────────────────────────┘
                     │ TCP (Protobuf 信令)
┌────────────────────┴────────────────────────────┐
│           C++ 信令服务器 (epoll + Reactor)        │
│  EventLoop │ EventLoopPool │ TcpServer            │
│  AuthService │ MeetingService │ UserManager       │
│  MySQL 连接池 │ 分片锁 │ Protobuf Codec         │
└─────────────────────────────────────────────────┘
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
| **Protobuf** | 4字节大端长度头 + 变长body | `codec.h`, `proto/` |
| **MySQL** | 连接池(RAII) + prepared stmt + 事务 | `connection_pool.h/cpp`, `user_repository` |
| **分片锁** | shared_mutex + 16分片 | `user_manager.h`, `tcp_server.h` |
| **Qt6** | 信号槽、自定义控件、QSS暗色主题 | `src/client/` |
| **CMake** | FetchContent + 条件编译 | 各层级 `CMakeLists.txt` |

## 项目结构

```
wemeet/
├── CMakeLists.txt              # 顶层构建
├── proto/                      # Protobuf 协议定义 (4文件)
│   ├── common/auth/meeting/signaling.proto
├── src/
│   ├── common/                 # 通用工具库
│   │   ├── buffer.h            #   - 移动语义 Buffer
│   │   ├── memory_pool.h/cpp   #   - 三级 TLS 内存池
│   │   ├── lockfree_queue.h    #   - 无锁 SPSC 队列
│   │   ├── thread_pool.h/cpp   #   - std::thread 线程池
│   │   └── logger.h/cpp        #   - 异步双缓冲日志
│   ├── network/                # Reactor 网络框架
│   │   ├── socket.h/cpp        #   - RAII Socket
│   │   ├── event_loop.h/cpp    #   - epoll ET 事件循环
│   │   ├── event_loop_pool.h/cpp # - One Loop Per Thread
│   │   ├── tcp_server.h/cpp    #   - Reactor TCP 服务器
│   │   ├── tcp_connection.h/cpp#   - shared_ptr 连接管理
│   │   └── codec.h             #   - Protobuf 编解码
│   ├── db/                     # MySQL 数据库层
│   │   ├── connection_pool.h/cpp  # - RAII 连接池
│   │   ├── user_repository.h/cpp  # - CRUD + 事务
│   │   └── meeting_repository.h/cpp
│   ├── server/                 # 信令服务器
│   │   ├── main.cpp            #   - 入口 + 信号处理
│   │   ├── signaling_server.h/cpp # - 消息分发
│   │   ├── auth_service.h/cpp  #   - SHA256 认证
│   │   ├── meeting_service.h/cpp  # - 会议室状态机
│   │   └── user_manager.h/cpp  #   - 16分片读写锁
│   └── client/                 # Qt6 客户端
│       ├── main.cpp            #   - 暗色主题入口
│       ├── main_window.h/cpp   #   - 信号槽页面切换
│       ├── login_dialog.h/cpp  #   - 登录/注册
│       ├── meeting_room.h/cpp  #   - 视频画廊 + 聊天
│       └── network_client.h/cpp#   - QTcpSocket 异步
├── tests/                      # 29 个单元测试
├── scripts/
│   ├── init_db.sql             # 数据库建表
│   └── start_server.sh         # 一键编译启动
├── docs/                       # 📖 学习文档（按模块拆分）
│   ├── 00-学习路线总览.md
│   ├── 01-通用工具库.md
│   ├── 02-协议层与编解码.md
│   ├── 03-网络框架.md
│   ├── 04-数据库层.md
│   ├── 05-信令服务器.md
│   ├── 06-Qt客户端.md
│   └── 07-单元测试.md
└── README.md
```

## 环境要求

| 组件 | 版本要求 | 说明 |
|------|----------|------|
| 操作系统 | Ubuntu 22.04+ (WSL2) 或原生 Linux | 推荐 Win11 + WSL2（自带 WSLg GUI 支持） |
| C++ 编译器 | GCC 11+ 或 Clang 14+ | 需要 C++17 支持 |
| CMake | 3.20+ | 模块化构建 |
| Protobuf | 3.12+ | 二进制协议 |
| Boost | 1.74+ | system 组件 |
| MySQL | 8.0+ | 数据库 |
| Qt6 | 6.2+ | 客户端 GUI（仅编译客户端时需要） |

## 快速开始

### 1. 安装依赖

```bash
# 首次需要更新 apt 源（拉取 universe 仓库中的 Qt6/protobuf-compiler 包列表）
sudo apt update

sudo apt install -y \
    build-essential cmake g++ \
    libprotobuf-dev protobuf-compiler \
    libboost-system-dev \
    libmysqlclient-dev \
    libssl-dev \
    qt6-base-dev qt6-multimedia-dev
```

> **WSL2 用户提示**：如果 Windows 上已安装 MySQL，无需在 WSL 中重复安装 `mysql-client`。可将 Windows 版 MySQL 客户端添加到 WSL 的 PATH：
> ```bash
> echo 'alias mysql='\''"/mnt/c/Program Files/MySQL/MySQL Server 8.0/bin/mysql.exe"'\''' >> ~/.bashrc
> source ~/.bashrc
> ```

### 2. 数据库初始化

```bash
# 使用 --db-pass 直接传密码（-p 与密码之间无空格）
mysql -u root -p你的密码 < scripts/init_db.sql

# 或先进入 MySQL 交互模式再执行文件
mysql -u root -p
# 进入 MySQL 后执行: source scripts/init_db.sql;
```

初始化脚本会创建 `wemeet` 数据库及 4 张表，并插入测试用户：
| 邮箱 | 密码 |
|------|------|
| alice@wemeet.com | 123456 |
| bob@wemeet.com | 123456 |
| carol@wemeet.com | 123456 |

### 3. 编译

```bash
cd wemeet
mkdir build && cd build

# 完整编译（服务端 + 客户端 + 测试）
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_NETWORK=ON \
    -DBUILD_DB=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_CLIENT=ON

cmake --build . -j$(nproc)
```

> 如果不需要客户端，去掉 `-DBUILD_CLIENT=ON` 即可。

### 4. 运行测试

```bash
./tests/wemeet_test
```

### 5. 启动信令服务器

```bash
# 新开一个终端，前台运行服务器
./build/src/server/wemeet_server --port 9090 --db-pass 你的MySQL密码
```

服务器支持的命令行参数：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 9090 | 监听端口 |
| `--ip` | 0.0.0.0 | 监听地址 |
| `--threads` | auto | 工作线程数 |
| `--db-host` | 127.0.0.1 | MySQL 地址 |
| `--db-port` | 3306 | MySQL 端口 |
| `--db-user` | root | MySQL 用户名 |
| `--db-pass` | (空) | MySQL 密码 |
| `--db-name` | wemeet | 数据库名 |

启动成功后会看到：
```
WeMeet Signaling Server v1.0.0
Listening on 0.0.0.0:9090
ConnectionPool created: 4/16 connections ready
SignalingServer initialized on 0.0.0.0:9090
EventLoopPool started: 16 worker threads
```

按 `Ctrl+C` 优雅关闭服务器。

### 6. 启动 Qt 客户端

```bash
./build/src/client/wemeet_client
```

- **Win11 + WSL2**：WSLg 原生支持 GUI，客户端窗口会直接在 Windows 桌面上显示，无需额外配置
- **原生 Linux**：直接运行即可
- **旧版 Win10 WSL1/2**：需要安装 X Server（如 [VcXsrv](https://sourceforge.net/projects/vcxsrv/)），并设置 `export DISPLAY=:0`

## 📖 学习文档

详细的模块学习指南见 [`docs/`](./docs/) 目录：

| 文档 | 内容 |
|------|------|
| [00-学习路线总览](./docs/00-学习路线总览.md) | 分层架构、技术点速查、10天学习计划 |
| [01-通用工具库](./docs/01-通用工具库.md) | Buffer移动语义、内存池TLS、无锁队列、线程池、双缓冲日志 |
| [02-协议层与编解码](./docs/02-协议层与编解码.md) | Protobuf协议设计、BaseMessage包装模式、粘包拆包解决方案 |
| [03-网络框架](./docs/03-网络框架.md) | epoll ET、Reactor模型、One Loop Per Thread、分片锁连接表 |
| [04-数据库层](./docs/04-数据库层.md) | RAII连接池、Prepared Statement、SQL注入防御、事务 |
| [05-信令服务器](./docs/05-信令服务器.md) | 消息分发、SHA256认证、Token机制、WebRTC信令转发 |
| [06-Qt客户端](./docs/06-Qt客户端.md) | 信号槽、QSS暗色主题、QTcpSocket异步通信 |
| [07-单元测试](./docs/07-单元测试.md) | 测试框架、29个测试用例、从测试倒推源码的学习方法 |

每个文档都包含：**为什么用这个技术**、**适用场景**、**代码逐行解析**、**学习检查点**。

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

### protobuf-compiler 找不到 / protoc executable not found
Ubuntu 26.04 中 `protobuf-compiler` 位于 `universe` 仓库，需要先 `sudo apt update` 拉取包列表。`libprotoc-dev` 仅提供编译库，不包含 `protoc` 可执行文件。

### Qt6 包找不到
同样需要 `apt update` 启用 universe 源。`qt6-base-dev` 和 `qt6-multimedia-dev` 均在 universe 仓库中。

### WSL2 中使用 Windows 上的 MySQL
如果 Windows 已安装 MySQL，可直接在 WSL 中调用：
```bash
"/mnt/c/Program Files/MySQL/MySQL Server 8.0/bin/mysql.exe" -u root -p < scripts/init_db.sql
```

### mysql -u root -p < file 无法输入密码
输入重定向 `<` 与交互式密码提示冲突，使用 `-p密码`（无空格）直接传参：
```bash
mysql -u root -p123456 < scripts/init_db.sql
```

### WSL2 客户端窗口不显示
- Win11 + WSL2 自带 WSLg，无需额外配置，直接运行 `./build/src/client/wemeet_client` 即可
- 确认 WSL 版本：`wsl --version`（需要 WSL 2.x+）
- 如仍无法显示，在 Windows 终端中运行 `wsl --update` 更新 WSL

### MySQL 连接池警告 MYSQL_OPT_RECONNECT
```
WARNING: MYSQL_OPT_RECONNECT is deprecated and will be removed in a future version.
```
这是 MySQL 8.x 的正常警告，不影响功能，连接池会通过 `mysql_options` 的替代方式处理重连。

## 统计数据

- **代码行数**: ~16,500 行 C++
- **文件数量**: 50+ 源文件
- **测试覆盖**: 29 个单元测试, 7 个测试套件
- **二进制大小**: 服务端 406KB / 客户端 123KB

## 简历描述

> **WeMeet — 企业级高性能视频会议系统** (C++17 | Qt6 | epoll | Protobuf | MySQL)
>
> - 基于 **Reactor + One Loop Per Thread** 架构实现信令服务器，epoll ET 边缘触发支持 C10K 并发，Protobuf 二进制协议降低序列化开销
> - 设计**三级 TLS 内存池**(8KB/64KB/1MB) + 无锁 SPSC 队列实现网络帧零拷贝传递，消除内存碎片与锁竞争
> - 实现 **MySQL RAII 连接池** + prepared statement + 事务管理，用户表 16 分片 `shared_mutex` 读写锁优化并发访问
> - 使用 **Qt6** 构建跨平台桌面客户端，信号槽机制实现线程安全 UI 更新，自定义视频画廊/音量控件，QSS 全局暗色主题
> - 全量 **RAII + 智能指针** 管理对象生命周期，CMake 模块化构建，29 项单元测试全覆盖，Valgrind/ASan 验证零泄漏
>
> 📄 **完整简历成品（可直投）**：见 [`RESUME.md`](./RESUME.md) —— 含技术栈映射表、难点与解决方案、Phase 1–5 技术路线、可量化成果、中英文简历文案与面试追问预案。
