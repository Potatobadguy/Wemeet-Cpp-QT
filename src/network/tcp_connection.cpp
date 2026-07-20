#include "tcp_connection.h"
#include "logger.h"
#include "codec.h"
#include <unistd.h>
#include <cstring>

namespace wemeet {

TcpConnection::TcpConnection(EventLoop* loop, Socket sock, uint64_t conn_id)
    : loop_(loop)
    , sock_(std::move(sock))
    , conn_id_(conn_id) {
    sock_.set_tcp_nodelay(true);
    sock_.set_keepalive(true);
    LOG_DEBUG("TcpConnection created: conn_id=%lu, fd=%d", conn_id_, sock_.fd());
}

TcpConnection::~TcpConnection() {
    LOG_DEBUG("TcpConnection destroyed: conn_id=%lu", conn_id_);
}

// ── 启动：注册到 EventLoop ──────────────────────────────
void TcpConnection::start() {
    // 注册读/写事件到子 EventLoop
    auto self = shared_from_this();
    loop_->add_read_event(sock_.fd(), [self]() {
        self->handle_read();
        // 写事件也由同一回调处理（同时检测 handle_write 状态）
        if (self->writing_.load(std::memory_order_acquire)) {
            self->handle_write();
        }
    });
    connected_.store(true, std::memory_order_release);
    LOG_DEBUG("TcpConnection started: conn_id=%lu, fd=%d", conn_id_, sock_.fd());
}

// ── 优雅关闭 ─────────────────────────────────────────────
void TcpConnection::shutdown() {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;

    loop_->run_in_loop([this]() {
        ::shutdown(sock_.fd(), SHUT_WR);   // 半关闭：不再发送
    });
}

void TcpConnection::force_close() {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;

    loop_->run_in_loop([this]() {
        sock_.close();
        if (close_cb_) {
            close_cb_(shared_from_this());
        }
    });
}

// ── 数据发送 ─────────────────────────────────────────────
void TcpConnection::send(const void* data, size_t len) {
    if (!connected_.load(std::memory_order_acquire)) return;

    // 使用内存池分配 → 移动到 Buffer
    Buffer buf;
    buf.append(data, len);
    send_in_loop(std::move(buf));
}

void TcpConnection::send(const std::string& msg) {
    send(msg.data(), msg.size());
}

void TcpConnection::send(Buffer&& buf) {
    if (!connected_.load(std::memory_order_acquire)) return;
    send_in_loop(std::move(buf));
}

void TcpConnection::send_in_loop(Buffer buf) {
    // shared_ptr 包装避免 std::function 拷贝构造限制
    auto buf_ptr = std::make_shared<Buffer>(std::move(buf));
    loop_->run_in_loop([self = shared_from_this(), buf_ptr]() {
        auto& buf = *buf_ptr;
        // 如果当前未在写且写缓冲区为空 → 直接写
        if (!self->writing_.load(std::memory_order_acquire) &&
            self->write_buf_.readable_size() == 0) {

            ssize_t n = ::write(self->sock_.fd(), buf.data(), buf.readable_size());
            if (n > 0) {
                LOG_DEBUG("SEND_DIRECT conn_id=%lu fd=%d: %zd of %zu bytes",
                          self->conn_id(), self->sock_.fd(), n, buf.readable_size());
                buf.retrieve(n);
                if (buf.readable_size() == 0) return;  // 写完
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("write error fd=%d: %s", self->sock_.fd(), strerror(errno));
                self->force_close();
                return;
            }
        }

        // 有剩余 → 追加到写缓冲，等待 EPOLLOUT
        if (buf.readable_size() > 0) {
            self->write_buf_.append(buf.data(), buf.readable_size());
            self->writing_.store(true, std::memory_order_release);
            self->loop_->enable_write(self->sock_.fd());
            LOG_DEBUG("SEND_BUFFERED conn_id=%lu fd=%d: %zu bytes queued",
                      self->conn_id(), self->sock_.fd(), buf.readable_size());
        }
    });
}

// ── I/O 处理 ─────────────────────────────────────────────
void TcpConnection::handle_read() {
    // 从 socket 读数据到缓冲区
    char extrabuf[65536];  // 栈上临时缓冲，避免小包频繁分配
    const size_t writable = read_buf_.writable_size();

    struct iovec vec[2];
    vec[0].iov_base = read_buf_.data() + read_buf_.readable_size();  // 实际位置由内部管理简化
    vec[0].iov_len  = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len  = sizeof(extrabuf);

    // 简化为直接 read
    ssize_t n = ::read(sock_.fd(), extrabuf, sizeof(extrabuf));

    if (n > 0) {
        read_buf_.append(extrabuf, n);
        LOG_DEBUG("RECV conn_id=%lu fd=%d: %zd bytes", conn_id_, sock_.fd(), n);

        // 尝试解码消息: 4字节大端长度头 + Protobuf body
        while (read_buf_.readable_size() >= 4) {
            uint32_t body_len = Codec::peek_length(read_buf_);
            if (body_len > 64 * 1024 * 1024) {  // 64MB 上限
                LOG_ERROR("Message too large: %u bytes", body_len);
                force_close();
                return;
            }

            if (read_buf_.readable_size() < 4 + body_len) break;  // 数据不完整

            // 完整消息 → 回调
            read_buf_.retrieve(4);  // 跳过长度头
            LOG_INFO("MSG_RECV conn_id=%lu body_len=%u", conn_id_, body_len);
            if (message_cb_) {
                message_cb_(shared_from_this(), read_buf_);
            }
            read_buf_.retrieve(body_len);
        }
    } else if (n == 0) {
        // 对端关闭
        LOG_DEBUG("Connection closed by peer: conn_id=%lu", conn_id_);
        force_close();
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("read error fd=%d: %s", sock_.fd(), strerror(errno));
            force_close();
        }
    }
}

void TcpConnection::handle_write() {
    if (!writing_.load(std::memory_order_acquire)) return;

    ssize_t n = ::write(sock_.fd(), write_buf_.data(), write_buf_.readable_size());
    if (n > 0) {
        write_buf_.retrieve(n);
        if (write_buf_.readable_size() == 0) {
            writing_.store(false, std::memory_order_release);
            loop_->disable_write(sock_.fd());
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("write error fd=%d: %s", sock_.fd(), strerror(errno));
        force_close();
    }
}

void TcpConnection::handle_error() {
    LOG_WARN("Connection error: conn_id=%lu", conn_id_);
    force_close();
}

} // namespace wemeet
