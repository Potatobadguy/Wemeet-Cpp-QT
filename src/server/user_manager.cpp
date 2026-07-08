#include "user_manager.h"
#include "logger.h"

namespace wemeet {

void UserManager::add_session(const SessionInfo& session) {
    size_t idx = shard_index(session.user_id);
    auto& shard = shards_[idx];

    std::unique_lock lock(shard.mutex);
    shard.sessions[session.user_id] = std::make_shared<SessionInfo>(session);
    online_count_.fetch_add(1, std::memory_order_release);

    LOG_INFO("User online: user_id=%lu, nickname=%s, total=%zu",
             session.user_id, session.nickname.c_str(), online_count_.load());
}

void UserManager::remove_session(uint64_t user_id) {
    size_t idx = shard_index(user_id);
    auto& shard = shards_[idx];

    std::unique_lock lock(shard.mutex);
    if (shard.sessions.erase(user_id) > 0) {
        online_count_.fetch_sub(1, std::memory_order_release);
        LOG_INFO("User offline: user_id=%lu, total=%zu",
                 user_id, online_count_.load());
    }
}

std::shared_ptr<SessionInfo> UserManager::get_session(uint64_t user_id) {
    size_t idx = shard_index(user_id);
    auto& shard = shards_[idx];

    // 读锁：多线程可并发读
    std::shared_lock lock(shard.mutex);
    auto it = shard.sessions.find(user_id);
    if (it != shard.sessions.end()) {
        return it->second;
    }
    return nullptr;
}

void UserManager::set_online(uint64_t user_id, bool online) {
    if (online) {
        LOG_DEBUG("User %lu status: online", user_id);
    } else {
        remove_session(user_id);
    }
}

} // namespace wemeet
