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
    // ET 边缘触发模式：内核只在"无数据 → 有数据"状态变化时通知一次。
    // 因此必须循环 read 直到 EAGAIN（读空内核缓冲区），否则剩余数据
    // 会因"可读状态未变化"而永久滞留，EPOLLIN 不再触发。
    char extrabuf[65536];  // 栈上临时缓冲，避免小包频繁分配

    while (true) {
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
            continue;  // 继续读，ET 下必须循环读到 EAGAIN
        } else if (n == 0) {
            // 对端关闭
            LOG_DEBUG("Connection closed by peer: conn_id=%lu", conn_id_);
            force_close();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 内核缓冲区已读空，正常退出本轮读取
            }
            LOG_ERROR("read error fd=%d: %s", sock_.fd(), strerror(errno));
            force_close();
            return;
        }
    }
}

void TcpConnection::handle_write() {
    if (!writing_.load(std::memory_order_acquire)) return;

    // ET 边缘触发模式：EPOLLOUT 同样只在"不可写 → 可写"状态变化时通知一次。
    // 必须循环 write 直到 EAGAIN（发送缓冲写满）或写空 write_buf_，
    // 保证停在"状态稳定点"，下次缓冲腾出空间时才会再次触发。
    while (write_buf_.readable_size() > 0) {
        ssize_t n = ::write(sock_.fd(), write_buf_.data(), write_buf_.readable_size());
        if (n > 0) {
            write_buf_.retrieve(n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 发送缓冲已满，保持 EPOLLOUT 挂起，等待下次可写通知
            }
            LOG_ERROR("write error fd=%d: %s", sock_.fd(), strerror(errno));
            force_close();
            return;
        } else {
            // write 返回 0 理论上不会发生（len>0），防御性退出避免死循环
            break;
        }
    }

    // 全部写完 → 关闭 EPOLLOUT；仍有剩余 → 保持监听等待下次可写触发
    if (write_buf_.readable_size() == 0) {
        writing_.store(false, std::memory_order_release);
        loop_->disable_write(sock_.fd());
    }
}

void TcpConnection::handle_error() {
    LOG_WARN("Connection error: conn_id=%lu", conn_id_);
    force_close();
}

} // namespace wemeet
