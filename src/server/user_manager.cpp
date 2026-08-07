#include "user_manager.h"
#include "logger.h"

namespace wemeet {

/**
 * @brief 新增/更新在线会话
 *
 * 实现要点（16 分片锁 + 写锁）：
 *  1. shard_index() 把 user_id 路由到 16 片之一；
 *  2. 只锁目标片（unique_lock = 独占写锁），其他 15 片的操作
 *     完全不受影响 → 并发写不同用户互不阻塞；
 *  3. 用 make_shared 创建堆上对象，返回值可安全跨线程传递；
 *  4. online_count_ 原子 +1（release 序：保证 map 写入对其他线程
 *     可见后再发布计数，避免"看到 +1 却查不到数据"）。
 *
 * 锁粒度说明：
 *  同一片的两个用户写操作仍会互斥（同一把锁），但跨片的写操作
 *  完全并行——这就是"把一把大锁拆成 16 把小锁"带来的并发收益。
 */
void UserManager::add_session(const SessionInfo& session) {
    // 路由到分片（取模，恒等映射，无需哈希）
    size_t idx = shard_index(session.user_id);
    auto& shard = shards_[idx];

    // 独享写锁：同一片内互斥，跨片无影响
    std::unique_lock lock(shard.mutex);
    shard.sessions[session.user_id] = std::make_shared<SessionInfo>(session);
    // 原子维护在线人数（覆盖场景不重复计数：先 +1）
    online_count_.fetch_add(1, std::memory_order_release);

    LOG_INFO("User online: user_id=%lu, nickname=%s, total=%zu",
             session.user_id, session.nickname.c_str(), online_count_.load());
}

/**
 * @brief 移除在线会话
 *
 * 与 add_session 对称：
 *  1. 路由到分片 → 2. 独占写锁 → 3. erase → 4. 命中才扣计数。
 *
 * 幂等性：用户不在线时 erase 返回 0，跳过 fetch_sub——
 * 保证"重复踢一个已离线用户"不会把在线人数扣成负数。
 */
void UserManager::remove_session(uint64_t user_id) {
    size_t idx = shard_index(user_id);
    auto& shard = shards_[idx];

    // 独占写锁（std::shared_mutex 的 unique_lock 是写锁）
    std::unique_lock lock(shard.mutex);
    // erase 返回被删除的元素个数：>0 才说明真的删除了
    if (shard.sessions.erase(user_id) > 0) {
        online_count_.fetch_sub(1, std::memory_order_release);
        LOG_INFO("User offline: user_id=%lu, total=%zu",
                 user_id, online_count_.load());
    }
}

/**
 * @brief 查询会话
 *
 * 读多写少场景的核心优化点：
 *  - 持【共享锁】（shared_lock）：多个线程可以同时调用本函数
 *    查询【不同或相同】分片，读读完全并行；
 *  - 只有"查询期间恰好有人在该片增/删"时才会短暂阻塞等写锁，
 *    而写操作远少于读操作，所以阻塞概率低、等待时间短。
 *
 * 返回 shared_ptr 的线程安全含义：
 *  - 查询线程拿到 shared_ptr 后，即使原会话被 remove_session 移除，
 *    对象仍存活（引用计数 +1），不会悬垂；
 *  - 但要注意：移除后该对象的数据是"旧快照"，业务层需自行判断
 *    会话是否已过期（例如通过 conn_id 是否仍有效）。
 */
std::shared_ptr<SessionInfo> UserManager::get_session(uint64_t user_id) {
    size_t idx = shard_index(user_id);
    auto& shard = shards_[idx];

    // 读锁：多线程可并发读（std::shared_mutex 的 shared_lock 是读锁）
    std::shared_lock lock(shard.mutex);
    auto it = shard.sessions.find(user_id);
    if (it != shard.sessions.end()) {
        return it->second;
    }
    return nullptr;
}

/**
 * @brief 更新在线状态
 *
 * 简化接口：online=true 不真正"上线"（上线动作由 add_session 完成，
 * 这里仅记录日志，避免与 add_session 产生状态歧义）；
 * online=false 时复用 remove_session 完成下线清理。
 */
void UserManager::set_online(uint64_t user_id, bool online) {
    if (online) {
        LOG_DEBUG("User %lu status: online", user_id);
    } else {
        remove_session(user_id);
    }
}

} // namespace wemeet
