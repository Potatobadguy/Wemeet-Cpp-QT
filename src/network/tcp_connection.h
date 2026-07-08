#pragma once
#include "event_loop.h"
#include "socket.h"
#include "buffer.h"
#include "memory_pool.h"
#include <any>
#include <memory>
#include <atomic>
#include <functional>

namespace wemeet {

class TcpServer;   // 前向声明

/**
 * @brief TCP 连接 — RAII 管理，shared_ptr 控制生命周期
 *
 * 技术亮点：
 *   - enable_shared_from_this 安全回调
 *   - 读写缓冲区集成内存池
 *   - 优雅关闭（shutdown → 等待 → close）
 *   - 用户自定义回调（lambda 捕获）
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback    = std::function<void(const std::shared_ptr<TcpConnection>&,
                                                   Buffer&)>;
    using CloseCallback      = ConnectionCallback;

    TcpConnection(EventLoop* loop, Socket sock, uint64_t conn_id);
    ~TcpConnection();

    // ── 生命周期管理 ─────────────────────────────────────
    void start();        // 注册到 EventLoop，开始监听
    void shutdown();     // 优雅关闭
    void force_close();  // 强制关闭

    // ── 数据发送 ─────────────────────────────────────────
    void send(const void* data, size_t len);
    void send(const std::string& msg);
    void send(Buffer&& buf);      // 移动语义零拷贝发送

    // ── 回调设置 ─────────────────────────────────────────
    void set_message_callback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void set_close_callback(CloseCallback cb)      { close_cb_ = std::move(cb); }

    // ── 属性 ─────────────────────────────────────────────
    uint64_t        conn_id()  const { return conn_id_; }
    EventLoop*      loop()     const { return loop_; }
    const Socket&   socket()   const { return sock_; }
    bool            connected() const { return connected_.load(std::memory_order_acquire); }

    // 上下文数据（用户自定义，如 user_id）
    void set_context(std::any ctx) { context_ = std::move(ctx); }
    template<typename T> T* get_context() { return std::any_cast<T>(&context_); }

private:
    void handle_read();
    void handle_write();
    void handle_error();
    void send_in_loop(Buffer buf);

    EventLoop*          loop_;
    Socket              sock_;
    uint64_t            conn_id_;

    // 读写缓冲区
    Buffer              read_buf_;
    Buffer              write_buf_;
    std::atomic<bool>   writing_{false};   // 是否正在写
    std::atomic<bool>   connected_{true};

    // 回调
    MessageCallback     message_cb_;
    CloseCallback       close_cb_;

    // 用户上下文
    std::any            context_;
};

} // namespace wemeet
