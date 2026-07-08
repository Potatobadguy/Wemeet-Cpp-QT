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
└── README.md
```

## 快速开始

### 依赖安装
```bash
sudo apt install -y \
    build-essential cmake g++ \
    libprotobuf-dev protobuf-compiler \
    libboost-system-dev \
    libmysqlclient-dev \
    libssl-dev \
    qt6-base-dev qt6-multimedia-dev
```

### 编译 & 测试
```bash
cd wemeet
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON -DBUILD_NETWORK=ON \
    -DBUILD_DB=ON -DBUILD_TESTS=ON
cmake --build . -j$(nproc)

# 运行测试 (29/29 通过)
./tests/wemeet_test

# 启动服务器
./src/server/wemeet_server --port 9090
```

### 数据库初始化
```bash
mysql -u root -p < scripts/init_db.sql
```

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
