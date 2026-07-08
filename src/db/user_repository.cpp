#include "user_repository.h"
#include "logger.h"
#include <cstring>

namespace wemeet {

std::optional<uint64_t> UserRepository::create(
        const std::string& email,
        const std::string& password_hash,
        const std::string& nickname) {

    auto guard = pool_->acquire();
    if (!guard.conn) return std::nullopt;

    const char* sql =
        "INSERT INTO users (email, password_hash, nickname) VALUES (?, ?, ?)";

    MYSQL_STMT* stmt = mysql_stmt_init(guard.conn);
    if (!stmt) {
        LOG_ERROR("mysql_stmt_init failed");
        return std::nullopt;
    }

    // 绑定参数
    MYSQL_BIND bind[3]{};
    unsigned long email_len    = email.size();
    unsigned long pass_len     = password_hash.size();
    unsigned long nick_len     = nickname.size();

    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer      = const_cast<char*>(email.c_str());
    bind[0].buffer_length = email_len;
    bind[0].length      = &email_len;

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer      = const_cast<char*>(password_hash.c_str());
    bind[1].buffer_length = pass_len;
    bind[1].length      = &pass_len;

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer      = const_cast<char*>(nickname.c_str());
    bind[2].buffer_length = nick_len;
    bind[2].length      = &nick_len;

    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) ||
        mysql_stmt_bind_param(stmt, bind)) {
        LOG_ERROR("mysql_stmt_prepare/bind failed: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    if (mysql_stmt_execute(stmt)) {
        LOG_ERROR("mysql_stmt_execute failed: %s", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return std::nullopt;
    }

    uint64_t id = mysql_stmt_insert_id(stmt);
    mysql_stmt_close(stmt);

    LOG_INFO("User created: id=%lu, email=%s", id, email.c_str());
    return id;
}

std::optional<UserInfo> UserRepository::find_by_email(const std::string& email) {
    auto guard = pool_->acquire();
    if (!guard.conn) return std::nullopt;

    char escaped[256];
    mysql_real_escape_string(guard.conn, escaped, email.c_str(), email.size());
    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, email, password_hash, nickname, COALESCE(avatar_url,''), status "
        "FROM users WHERE email = '%s' LIMIT 1", escaped);

    if (mysql_query(guard.conn, query)) {
        LOG_ERROR("query failed: %s", mysql_error(guard.conn));
        return std::nullopt;
    }

    MYSQL_RES* result = mysql_store_result(guard.conn);
    if (!result) return std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) {
        mysql_free_result(result);
        return std::nullopt;
    }

    UserInfo user;
    user.id            = row[0] ? std::stoull(row[0]) : 0;
    user.email         = row[1] ? row[1] : "";
    user.password_hash = row[2] ? row[2] : "";
    user.nickname      = row[3] ? row[3] : "";
    user.avatar_url    = row[4] ? row[4] : "";
    user.status        = row[5] ? static_cast<int8_t>(std::stoi(row[5])) : 0;

    mysql_free_result(result);
    return user;
}

std::optional<UserInfo> UserRepository::find_by_id(uint64_t user_id) {
    auto guard = pool_->acquire();
    if (!guard.conn) return std::nullopt;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, email, password_hash, nickname, COALESCE(avatar_url,''), status "
        "FROM users WHERE id = %lu LIMIT 1", user_id);

    if (mysql_query(guard.conn, query)) return std::nullopt;

    MYSQL_RES* result = mysql_store_result(guard.conn);
    if (!result) return std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return std::nullopt; }

    UserInfo user;
    user.id            = row[0] ? std::stoull(row[0]) : 0;
    user.email         = row[1] ? row[1] : "";
    user.password_hash = row[2] ? row[2] : "";
    user.nickname      = row[3] ? row[3] : "";
    user.avatar_url    = row[4] ? row[4] : "";
    user.status        = row[5] ? static_cast<int8_t>(std::stoi(row[5])) : 0;

    mysql_free_result(result);
    return user;
}

bool UserRepository::update_status(uint64_t user_id, int8_t status) {
    auto guard = pool_->acquire();
    if (!guard.conn) return false;

    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE users SET status = %d WHERE id = %lu", status, user_id);

    if (mysql_query(guard.conn, query)) {
        LOG_ERROR("update_status failed: %s", mysql_error(guard.conn));
        return false;
    }
    return mysql_affected_rows(guard.conn) > 0;
}

std::vector<UserInfo> UserRepository::get_friends(uint64_t user_id) {
    auto guard = pool_->acquire();
    if (!guard.conn) return {};

    char query[1024];
    snprintf(query, sizeof(query),
        "SELECT u.id, u.email, u.password_hash, u.nickname, "
        "COALESCE(u.avatar_url,''), u.status "
        "FROM friendships f JOIN users u ON f.friend_id = u.id "
        "WHERE f.user_id = %lu", user_id);

    if (mysql_query(guard.conn, query)) return {};

    MYSQL_RES* result = mysql_store_result(guard.conn);
    if (!result) return {};

    std::vector<UserInfo> friends;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        UserInfo u;
        u.id         = row[0] ? std::stoull(row[0]) : 0;
        u.email      = row[1] ? row[1] : "";
        u.nickname   = row[3] ? row[3] : "";
        u.avatar_url = row[4] ? row[4] : "";
        u.status     = row[5] ? static_cast<int8_t>(std::stoi(row[5])) : 0;
        friends.push_back(u);
    }

    mysql_free_result(result);
    return friends;
}

bool UserRepository::add_friend(uint64_t user_id, uint64_t friend_id) {
    auto guard = pool_->acquire();
    if (!guard.conn) return false;

    // 事务: 双向插入 + FOR UPDATE 防死锁
    mysql_query(guard.conn, "START TRANSACTION");

    // 统一加锁顺序（先小后大），避免死锁
    (void)std::min(user_id, friend_id);
    (void)std::max(user_id, friend_id);

    char query[512];
    snprintf(query, sizeof(query),
        "INSERT INTO friendships (user_id, friend_id) VALUES (%lu, %lu) "
        "ON DUPLICATE KEY UPDATE user_id=user_id", user_id, friend_id);

    if (mysql_query(guard.conn, query)) {
        mysql_query(guard.conn, "ROLLBACK");
        return false;
    }

    snprintf(query, sizeof(query),
        "INSERT INTO friendships (user_id, friend_id) VALUES (%lu, %lu) "
        "ON DUPLICATE KEY UPDATE user_id=user_id", friend_id, user_id);

    if (mysql_query(guard.conn, query)) {
        mysql_query(guard.conn, "ROLLBACK");
        return false;
    }

    mysql_query(guard.conn, "COMMIT");
    return true;
}

} // namespace wemeet
