#include "tcp_server.h"
#include "logger.h"
#include <cstring>

namespace wemeet {

TcpServer::TcpServer(EventLoop* main_loop, EventLoopPool* pool,
                     const std::string& ip, uint16_t port)
    : main_loop_(main_loop)
    , pool_(pool)
    , ip_(ip)
    , port_(port) {

    // 创建监听 socket
    listen_sock_ = Socket::create_tcp();
    listen_sock_.set_reuse_addr(true);
    listen_sock_.set_reuse_port(true);

    auto addr = Socket::make_address(ip, port);
    listen_sock_.bind(addr);
    listen_sock_.listen();

    LOG_INFO("TcpServer created: %s:%u, fd=%d", ip.c_str(), port, listen_sock_.fd());
}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    // EventLoop 的 add_read_event 不存储回调，
    // 改用定时轮询（50ms间隔）检查新连接
    main_loop_->run_every(50, [this]() {
        int count = 0;
        while (true) {
            sockaddr_in peer_addr;
            Socket conn_sock = listen_sock_.accept(&peer_addr);
            if (!conn_sock.valid()) break;
            count++;
            handle_accept(std::move(conn_sock), peer_addr);
        }
        if (count > 0) {
            LOG_INFO("Accept loop: accepted %d new connection(s) this tick", count);
        }
    });
    LOG_INFO("TcpServer starting on %s:%u, fd=%d", ip_.c_str(), port_, listen_sock_.fd());
}

void TcpServer::stop() {
    listen_sock_.close();
    LOG_INFO("TcpServer stopped");
}

// ── accept 新连接 ────────────────────────────────────────
void TcpServer::handle_accept(Socket conn_sock, const sockaddr_in& peer_addr) {
    if (!conn_sock.valid()) return;

    uint64_t conn_id = next_conn_id_.fetch_add(1, std::memory_order_relaxed);

    char ip_buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
    LOG_INFO("New connection: conn_id=%lu, ip=%s:%d",
             conn_id, ip_buf, ntohs(peer_addr.sin_port));

    // 分发到子 Reactor（轮询）
    EventLoop* io_loop = pool_->next_loop();

    // 创建连接并注册回调
    auto conn = std::make_shared<TcpConnection>(io_loop, std::move(conn_sock), conn_id);

    conn->set_message_callback(msg_cb_);

    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& closed_conn) {
        uint64_t id = closed_conn->conn_id();
        size_t shard_idx = id % kShardCount;
        {
            std::lock_guard<std::mutex> lock(shards_[shard_idx].mutex);
            shards_[shard_idx].connections.erase(id);
        }
        LOG_DEBUG("Connection removed: conn_id=%lu (total=%zu)", id, connection_count());
    });

    // 注册到分片连接表
    size_t shard_idx = conn_id % kShardCount;
    {
        std::lock_guard<std::mutex> lock(shards_[shard_idx].mutex);
        shards_[shard_idx].connections[conn_id] = conn;
    }

    conn->start();

    if (conn_cb_) {
        conn_cb_(conn);
    }
}

// ── 统计 ─────────────────────────────────────────────────
size_t TcpServer::connection_count() const {
    size_t total = 0;
    for (size_t i = 0; i < kShardCount; ++i) {
        std::lock_guard<std::mutex> lock(
            const_cast<std::mutex&>(shards_[i].mutex));
        total += shards_[i].connections.size();
    }
    return total;
}

// ── 广播 ─────────────────────────────────────────────────
void TcpServer::broadcast(const void* data, size_t len) {
    for (size_t i = 0; i < kShardCount; ++i) {
        std::lock_guard<std::mutex> lock(shards_[i].mutex);
        for (auto& [id, conn] : shards_[i].connections) {
            if (conn->connected()) {
                conn->send(data, len);
            }
        }
    }
}

std::shared_ptr<TcpConnection> TcpServer::get_connection(uint64_t conn_id) {
    size_t shard_idx = conn_id % kShardCount;
    std::lock_guard<std::mutex> lock(shards_[shard_idx].mutex);
    auto it = shards_[shard_idx].connections.find(conn_id);
    if (it != shards_[shard_idx].connections.end()) {
        return it->second;
    }
    return nullptr;
}

void TcpServer::send_to_conn(uint64_t conn_id, const void* data, size_t len) {
    auto conn = get_connection(conn_id);
    if (conn && conn->connected()) {
        conn->send(data, len);
    }
}

} // namespace wemeet
