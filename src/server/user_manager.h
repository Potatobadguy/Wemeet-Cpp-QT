#pragma once
#include <string>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <memory>
#include <atomic>
#include <cstdint>

namespace wemeet {

/**
 * @brief 在线用户管理器 — 16 分片锁，读多写少优化
 *
 * 技术亮点：
 *   - shared_mutex 读写锁：读操作（查找）共享锁，写操作独占锁
 *   - 16 分片减少锁竞争
 *   - 原子计数器跟踪在线人数
 */
struct SessionInfo {
    uint64_t    user_id;
    std::string nickname;
    std::string token;
    uint64_t    conn_id;     // 关联的 TCP 连接 ID
    int64_t     login_time;
};

class UserManager {
public:
    UserManager() = default;

    // ── 会话管理 ─────────────────────────────────────────
    void add_session(const SessionInfo& session);
    void remove_session(uint64_t user_id);
    std::shared_ptr<SessionInfo> get_session(uint64_t user_id);

    // 更新在线状态
    void set_online(uint64_t user_id, bool online);

    // ── 统计 ─────────────────────────────────────────────
    size_t online_count() const {
        return online_count_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t kShardCount = 16;

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, std::shared_ptr<SessionInfo>> sessions;
    };

    size_t shard_index(uint64_t user_id) const {
        return user_id % kShardCount;
    }

    std::array<Shard, kShardCount> shards_;
    std::atomic<size_t>            online_count_{0};
};

} // namespace wemeet
