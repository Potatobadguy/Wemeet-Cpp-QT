#pragma once
#include <mysql/mysql.h>
#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <functional>
#include <atomic>
#include <chrono>

namespace wemeet {

/**
 * @brief MySQL 连接池 — RAII 管理连接生命周期
 *
 * 技术亮点：
 *   - unique_ptr<MYSQL> 自动管理连接释放
 *   - 双端队列 + 条件变量 实现生产者-消费者
 *   - 空闲超时回收 + 最大连接数限制
 *   - 连接健康检查
 */
class ConnectionPool {
public:
    struct Config {
        std::string host     = "127.0.0.1";
        int         port     = 3306;
        std::string user     = "root";
        std::string password = "";
        std::string database = "wemeet";
        size_t      max_connections = 16;
        size_t      min_connections = 4;
        int         idle_timeout_sec = 300;
    };

    explicit ConnectionPool(const Config& config);
    ~ConnectionPool();

    // 不可拷贝
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // ── 获取/归还连接 ────────────────────────────────────
    // RAII 风格: 获取智能指针包装的连接
    struct ConnGuard {
        MYSQL*              conn;
        ConnectionPool*     pool;
        ConnGuard(MYSQL* c, ConnectionPool* p) : conn(c), pool(p) {}
        ~ConnGuard() { if (conn) pool->release(conn); }
        ConnGuard(ConnGuard&& o) noexcept
            : conn(o.conn), pool(o.pool) { o.conn = nullptr; }
        ConnGuard(const ConnGuard&) = delete;
        MYSQL* operator->() { return conn; }
        MYSQL& operator*()  { return *conn; }
    };

    ConnGuard acquire();   // 获取连接（可能阻塞）
    void release(MYSQL* conn);

    // ── 统计 ─────────────────────────────────────────────
    size_t active_count() const   { return active_count_.load(); }
    // idle_conns_ 在 acquire()/release() 中于锁内修改，此处读取同样加锁（#10）
    size_t idle_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_conns_.size();
    }

private:
    MYSQL* create_connection();
    void   destroy_connection(MYSQL* conn);
    bool   check_connection(MYSQL* conn);

    Config                      config_;
    std::deque<MYSQL*>          idle_conns_;
    mutable std::mutex          mutex_;   // mutable：供 const 统计方法加锁
    std::condition_variable     cv_;
    std::atomic<size_t>         total_count_{0};
    std::atomic<size_t>         active_count_{0};
};

} // namespace wemeet
