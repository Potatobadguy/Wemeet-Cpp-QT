#include "connection_pool.h"
#include "logger.h"
#include <cstring>
#include <thread>

namespace wemeet {

ConnectionPool::ConnectionPool(const Config& config)
    : config_(config) {

    // 预创建最小连接数
    for (size_t i = 0; i < config_.min_connections; ++i) {
        MYSQL* conn = create_connection();
        if (conn) {
            idle_conns_.push_back(conn);
            total_count_.fetch_add(1, std::memory_order_release);
        }
    }

    LOG_INFO("ConnectionPool created: %zu/%zu connections ready",
             idle_conns_.size(), config_.max_connections);
}

ConnectionPool::~ConnectionPool() {
    for (MYSQL* conn : idle_conns_) {
        destroy_connection(conn);
    }
    idle_conns_.clear();
    LOG_INFO("ConnectionPool destroyed: %zu connections released",
             total_count_.load());
}

// ── 获取连接（RAII Guard）─────────────────────────────────
ConnectionPool::ConnGuard ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待空闲连接或可创建新连接
    cv_.wait(lock, [this] {
        return !idle_conns_.empty() ||
               total_count_.load() < config_.max_connections;
    });

    MYSQL* conn = nullptr;

    if (!idle_conns_.empty()) {
        // 从空闲池获取
        conn = idle_conns_.front();
        idle_conns_.pop_front();
    } else {
        // 创建新连接
        lock.unlock();  // 创建连接可能耗时，先释放锁
        conn = create_connection();
        lock.lock();
        if (conn) {
            total_count_.fetch_add(1, std::memory_order_release);
        }
    }

    if (conn) {
        active_count_.fetch_add(1, std::memory_order_release);
    }

    return ConnGuard(conn, this);
}

void ConnectionPool::release(MYSQL* conn) {
    if (!conn) return;

    active_count_.fetch_sub(1, std::memory_order_release);

    std::lock_guard<std::mutex> lock(mutex_);

    if (check_connection(conn)) {
        idle_conns_.push_back(conn);
    } else {
        destroy_connection(conn);
        total_count_.fetch_sub(1, std::memory_order_release);
    }

    cv_.notify_one();
}

// ── 创建/销毁连接 ────────────────────────────────────────
MYSQL* ConnectionPool::create_connection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        LOG_ERROR("mysql_init failed");
        return nullptr;
    }

    // 连接超时设置
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);

    // 自动重连
    bool reconnect = true;
    mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

    if (!mysql_real_connect(conn, config_.host.c_str(), config_.user.c_str(),
                             config_.password.c_str(), config_.database.c_str(),
                             config_.port, nullptr, 0)) {
        LOG_ERROR("mysql_real_connect failed: %s", mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }

    LOG_DEBUG("MySQL connection created: %p", static_cast<void*>(conn));
    return conn;
}

void ConnectionPool::destroy_connection(MYSQL* conn) {
    if (conn) {
        mysql_close(conn);
        LOG_DEBUG("MySQL connection destroyed");
    }
}

bool ConnectionPool::check_connection(MYSQL* conn) {
    // mysql_ping 自动尝试重连
    return mysql_ping(conn) == 0;
}

} // namespace wemeet
