#pragma once
#include "event_loop_pool.h"
#include "tcp_connection.h"
#include "socket.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>

namespace wemeet {

/**
 * @brief Reactor TCP 服务器 — 主 Reactor accept + 子 Reactor I/O
 *
 * 技术亮点：
 *   - accept 在主 Reactor，连接分发到子 Reactor（轮询）
 *   - shared_ptr 管理连接生命周期
 *   - 可自定义连接/消息/关闭回调
 */
class TcpServer {
public:
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback    = TcpConnection::MessageCallback;

    TcpServer(EventLoop* main_loop, EventLoopPool* pool,
              const std::string& ip, uint16_t port);
    ~TcpServer();

    // 不可拷贝
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // ── 生命周期 ─────────────────────────────────────────
    void start();
    void stop();

    // ── 回调 ─────────────────────────────────────────────
    void set_connection_callback(ConnectionCallback cb) { conn_cb_ = std::move(cb); }
    void set_message_callback(MessageCallback cb)       { msg_cb_ = std::move(cb); }

    // ── 连接管理 ─────────────────────────────────────────
    size_t connection_count() const;

    // 向所有连接广播消息
    void broadcast(const void* data, size_t len);

    // 向指定 conn_id 的连接发送
    void send_to_conn(uint64_t conn_id, const void* data, size_t len);

    // 查找指定 conn_id 的连接
    std::shared_ptr<TcpConnection> get_connection(uint64_t conn_id);

private:
    void handle_accept();

    EventLoop*          main_loop_;
    EventLoopPool*      pool_;
    Socket              listen_sock_;
    std::string         ip_;
    uint16_t            port_;

    ConnectionCallback  conn_cb_;
    MessageCallback     msg_cb_;

    // 连接管理（分片锁，减少竞争）
    static constexpr size_t kShardCount = 16;
    struct Shard {
        std::mutex mutex;
        std::map<uint64_t, std::shared_ptr<TcpConnection>> connections;
    };
    std::array<Shard, kShardCount> shards_;
    std::atomic<uint64_t>          next_conn_id_{1};
};

} // namespace wemeet
