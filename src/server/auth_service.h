#pragma once
#include "connection_pool.h"
#include "user_repository.h"
#include "user_manager.h"
#include <string>
#include <optional>
#include <cstdint>

namespace wemeet {

/**
 * @brief 认证服务 — 登录/注册/Token 管理
 */
class AuthService {
public:
    AuthService(ConnectionPool* db_pool, UserManager* user_mgr)
        : user_repo_(db_pool), user_mgr_(user_mgr) {}

    // ── 注册 ─────────────────────────────────────────────
    struct RegisterResult {
        bool     success;
        uint64_t user_id;
        std::string error;
    };

    RegisterResult register_user(const std::string& email,
                                  const std::string& password,
                                  const std::string& nickname);

    // ── 登录 ─────────────────────────────────────────────
    struct LoginResult {
        bool     success;
        uint64_t user_id;
        std::string token;
        std::string nickname;
        std::string avatar_url;
        std::string error;
    };

    LoginResult login(const std::string& email,
                      const std::string& password,
                      uint64_t conn_id);

    // ── 登出 ─────────────────────────────────────────────
    void logout(uint64_t user_id, const std::string& token);

    // ── Token 验证 ───────────────────────────────────────
    struct TokenInfo {
        uint64_t user_id;
        std::string nickname;
    };
    std::optional<TokenInfo> validate_token(const std::string& token);

private:
    std::string generate_token(uint64_t user_id);
    std::string hash_password(const std::string& password);  // bcrypt 简化版

    UserRepository  user_repo_;
    UserManager*    user_mgr_;
};

} // namespace wemeet
