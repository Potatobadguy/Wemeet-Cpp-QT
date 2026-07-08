#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <system_error>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

namespace wemeet {

/**
 * @brief RAII Socket 封装 — 自动管理文件描述符生命周期
 *
 * 技术亮点：
 *   - RAII：构造打开，析构自动关闭
 *   - 移动语义：支持所有权转移（禁止拷贝）
 *   - 非阻塞模式设置
 *   - TCP_NODELAY / SO_REUSEADDR 调优
 */
class Socket {
public:
    static constexpr int kInvalidFd = -1;

    // ── 构造/析构 ───────────────────────────────────────
    Socket() : fd_(kInvalidFd) {}

    explicit Socket(int fd) : fd_(fd) {
        set_nonblocking(true);
    }

    ~Socket() { close(); }

    // 移动语义
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = kInvalidFd;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = kInvalidFd;
        }
        return *this;
    }

    // 禁止拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // ── 工厂方法 ─────────────────────────────────────────
    static Socket create_tcp() {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket()");
        }
        return Socket(fd);
    }

    static Socket create_udp() {
        int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket(udp)");
        }
        return Socket(fd);
    }

    // ── Socket 选项 ──────────────────────────────────────
    void set_reuse_addr(bool on) {
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    void set_reuse_port(bool on) {
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    void set_tcp_nodelay(bool on) {
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    void set_keepalive(bool on) {
        int opt = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    }

    void set_nonblocking(bool on) {
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (on)
            flags |= O_NONBLOCK;
        else
            flags &= ~O_NONBLOCK;
        ::fcntl(fd_, F_SETFL, flags);
    }

    // ── 操作 ─────────────────────────────────────────────
    void bind(const struct sockaddr_in& addr) {
        int ret = ::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (ret < 0) {
            throw std::system_error(errno, std::generic_category(), "bind()");
        }
    }

    void listen(int backlog = 128) {
        int ret = ::listen(fd_, backlog);
        if (ret < 0) {
            throw std::system_error(errno, std::generic_category(), "listen()");
        }
    }

    Socket accept(struct sockaddr_in* peer_addr = nullptr) {
        sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int conn_fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&addr),
                                &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (conn_fd < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                return Socket();  // 返回 invalid socket
            }
            throw std::system_error(err, std::generic_category(), "accept4()");
        }
        if (peer_addr) *peer_addr = addr;
        return Socket(conn_fd);
    }

    ssize_t read(void* buf, size_t len) {
        return ::read(fd_, buf, len);
    }

    ssize_t write(const void* buf, size_t len) {
        return ::write(fd_, buf, len);
    }

    void close() {
        if (fd_ != kInvalidFd) {
            ::close(fd_);
            fd_ = kInvalidFd;
        }
    }

    // ── 属性 ─────────────────────────────────────────────
    int fd() const { return fd_; }
    bool valid() const { return fd_ != kInvalidFd; }

    static sockaddr_in make_address(const std::string& ip, uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (ip.empty() || ip == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        }
        return addr;
    }

private:
    int fd_;
};

} // namespace wemeet
