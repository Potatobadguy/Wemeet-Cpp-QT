#pragma once
#include "connection_pool.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace wemeet {

/**
 * @brief 用户数据仓库 — CRUD 操作封装
 *
 * 技术亮点：
 *   - prepared statement 防 SQL 注入
 *   - RAII 连接管理（ConnGuard 自动归还）
 *   - 索引利用：email 唯一索引加速登录
 */
struct UserInfo {
    uint64_t    id      = 0;
    std::string email;
    std::string password_hash;
    std::string nickname;
    std::string avatar_url;
    int8_t      status  = 0;   // 0:离线 1:在线
};

class UserRepository {
public:
    explicit UserRepository(ConnectionPool* pool) : pool_(pool) {}

    // ── CRUD ──────────────────────────────────────────────
    // 创建用户
    std::optional<uint64_t> create(const std::string& email,
                                    const std::string& password_hash,
                                    const std::string& nickname);

    // 根据 email 查找（利用唯一索引）
    std::optional<UserInfo> find_by_email(const std::string& email);

    // 根据 ID 查找
    std::optional<UserInfo> find_by_id(uint64_t user_id);

    // 更新在线状态
    bool update_status(uint64_t user_id, int8_t status);

    // 获取好友列表（事务: 两次 SELECT）
    std::vector<UserInfo> get_friends(uint64_t user_id);

    // 添加好友关系（事务: 双向插入）
    bool add_friend(uint64_t user_id, uint64_t friend_id);

private:
    ConnectionPool* pool_;
};

} // namespace wemeet
