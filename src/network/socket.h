#pragma once                                                  // 防止头文件被重复包含
#include <cstdint>                                             // uint16_t 等定长整数类型
#include <string>                                              // std::string IP 地址字符串
#include <memory>                                              // 智能指针（预留，当前未使用）
#include <system_error>                                        // std::system_error 异常
#include <sys/socket.h>                                        // socket / bind / listen / setsockopt 等核心函数
#include <netinet/in.h>                                        // sockaddr_in / INADDR_ANY / htons
#include <netinet/tcp.h>                                       // TCP_NODELAY 选项
#include <arpa/inet.h>                                         // inet_pton 地址转换
#include <fcntl.h>                                             // fcntl 非阻塞标志
#include <unistd.h>                                            // ::close / ::read / ::write

namespace wemeet {

/**
 * @brief RAII Socket 封装 — 自动管理文件描述符生命周期
 *        （构造函数打开 fd，析构函数自动关闭，防止资源泄漏）
 *
 * 技术亮点：
 *   - RAII：构造打开，析构自动关闭
 *   - 移动语义：支持所有权转移（禁止拷贝）
 *   - 非阻塞模式设置
 *   - TCP_NODELAY / SO_REUSEADDR 调优
 */
class Socket {
public:
    static constexpr int kInvalidFd = -1;    // 无效文件描述符常量（-1 是 POSIX 惯例）

    // ── 构造/析构 ───────────────────────────────────────
    Socket() : fd_(kInvalidFd) {}            // 默认构造：初始化为无效 fd，不打开任何系统资源

    explicit Socket(int fd) : fd_(fd) {      // 从已有 fd 构造，接管该 fd 的所有权
        set_nonblocking(true);               // 默认设为非阻塞模式（配合 epoll 使用）
    }

    ~Socket() { close(); }                   // 析构时自动关闭文件描述符，确保不泄漏

    // 移动语义
    Socket(Socket&& other) noexcept : fd_(other.fd_) {  // 移动构造：接管 other 的 fd
        other.fd_ = kInvalidFd;                         // 将 other 置为无效，防止析构时重复关闭
    }

    Socket& operator=(Socket&& other) noexcept {        // 移动赋值
        if (this != &other) {                           // 自我赋值检查
            close();                                     // 先关闭当前持有的 fd
            fd_ = other.fd_;                             // 接管 other 的 fd
            other.fd_ = kInvalidFd;                      // other 放弃所有权
        }
        return *this;
    }

    // 禁止拷贝
    Socket(const Socket&) = delete;            // 禁止拷贝构造 — fd 是独占资源
    Socket& operator=(const Socket&) = delete; // 禁止拷贝赋值

    // ── 工厂方法 ─────────────────────────────────────────
    static Socket create_tcp() {                                                   // 创建 TCP socket
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); // IPv4 TCP + 非阻塞 + 子进程关闭
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket()");   // 失败则抛异常
        }
        return Socket(fd);     // 用工厂函数创建的 fd 构造 Socket 对象
    }

    static Socket create_udp() {                                                    // 创建 UDP socket
        int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);  // IPv4 UDP + 非阻塞
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket(udp)");
        }
        return Socket(fd);
    }

    // ── Socket 选项 ──────────────────────────────────────
    void set_reuse_addr(bool on) {                             // 设置 SO_REUSEADDR（地址重用，允许 TIME_WAIT 后立即绑定）
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    void set_reuse_port(bool on) {                             // 设置 SO_REUSEPORT（端口重用，支持多进程监听同端口）
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    void set_tcp_nodelay(bool on) {                            // 设置 TCP_NODELAY（禁用 Nagle 算法，降低延迟）
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    void set_keepalive(bool on) {                              // 设置 SO_KEEPALIVE（TCP 心跳保活检测）
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    }

    void set_nonblocking(bool on) {                            // 设置/取消 O_NONBLOCK 非阻塞标志
        int flags = ::fcntl(fd_, F_GETFL, 0);                  // 获取当前文件状态标志
        if (on)
            flags |= O_NONBLOCK;                               // 添加非阻塞标志
        else
            flags &= ~O_NONBLOCK;                              // 移除非阻塞标志
        ::fcntl(fd_, F_SETFL, flags);                          // 写回新标志
    }

    // ── 操作 ─────────────────────────────────────────────
    void bind(const struct sockaddr_in& addr) {                             // 将 socket 绑定到指定地址和端口
        int ret = ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (ret < 0) {
            throw std::system_error(errno, std::generic_category(), "bind()");
        }
    }

    void listen(int backlog = 128) {                    // 开始监听，backlog 为等待队列最大长度（默认 128）
        int ret = ::listen(fd_, backlog);
        if (ret < 0) {
            throw std::system_error(errno, std::generic_category(), "listen()");
        }
    }

    Socket accept(struct sockaddr_in* peer_addr = nullptr) {               // 接受客户端连接
        sockaddr_in addr;                                                   // 存放对端地址信息
        socklen_t addrlen = sizeof(addr);                                   // 地址结构体长度（输入输出参数）
        int conn_fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr),   // accept4 比 accept 多一个 flags 参数
                                &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);   // 新连接直接设为非阻塞 + 执行时关闭
        if (conn_fd < 0) {
            int err = errno;                            // 立即保存 errno，后续可能被其他操作覆盖
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {  // 无新连接（非阻塞）或被信号中断
                return Socket();                        // 返回空 Socket（valid() == false）
            }
            throw std::system_error(err, std::generic_category(), "accept4()");
        }
        if (peer_addr) *peer_addr = addr;               // 如果调用方传入了指针，则回填对端地址
        return Socket(conn_fd);                          // 用接受的 fd 构造新 Socket 返回给调用方
    }

    ssize_t read(void* buf, size_t len) {                // 从 socket 读取数据（非阻塞下返回 -1 + EAGAIN）
        return ::read(fd_, buf, len);
    }

    ssize_t write(const void* buf, size_t len) {         // 向 socket 写入数据
        return ::write(fd_, buf, len);
    }

    void close() {                                       // 手动关闭 socket
        if (fd_ != kInvalidFd) {                         // 只有持有有效 fd 才需要关闭
            ::close(fd_);                                // 关闭系统文件描述符
            fd_ = kInvalidFd;                            // 重置为无效，防止重复关闭
        }
    }

    // ── 属性 ─────────────────────────────────────────────
    int fd() const { return fd_; }               // 返回原始文件描述符（供底层系统调用使用）
    bool valid() const { return fd_ != kInvalidFd; }  // 判断 socket 是否持有有效的 fd

    static sockaddr_in make_address(const std::string& ip, uint16_t port) {  // 工厂：从 IP 字符串和端口构造 sockaddr_in
        sockaddr_in addr{};
        addr.sin_family = AF_INET;                      // IPv4 地址族
        addr.sin_port = htons(port);                    // 端口号转网络字节序（大端）
        if (ip.empty() || ip == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;          // 通配地址 0.0.0.0，监听所有网卡
        } else {
            ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);  // 将点分十进制 IP 转为二进制
        }
        return addr;
    }

private:
    int fd_;   // 底层套接字文件描述符（-1 表示无效 / 未持有）
};

} // namespace wemeet
