#include "auth_service.h"
#include "logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace wemeet {

// ── 简单的 SHA256 哈希 ──────────────────────────────────
std::string AuthService::hash_password(const std::string& password) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()),
           password.size(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// ── Token 生成 ──────────────────────────────────────────
std::string AuthService::generate_token(uint64_t user_id) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << std::hex << user_id << "_" << dist(gen) << "_" << dist(gen);
    return oss.str();
}

// ── 注册 ─────────────────────────────────────────────────
AuthService::RegisterResult AuthService::register_user(
        const std::string& email,
        const std::string& password,
        const std::string& nickname) {

    RegisterResult result{false, 0, ""};

    // 检查邮箱是否已注册
    auto existing = user_repo_.find_by_email(email);
    if (existing.has_value()) {
        result.error = "Email already registered";
        return result;
    }

    // 哈希密码
    std::string hashed = hash_password(password);

    // 写入数据库
    auto user_id = user_repo_.create(email, hashed, nickname);
    if (!user_id.has_value()) {
        result.error = "Database error";
        return result;
    }

    result.success = true;
    result.user_id = *user_id;

    LOG_INFO("User registered: id=%lu, email=%s", *user_id, email.c_str());
    return result;
}

// ── 登录 ─────────────────────────────────────────────────
AuthService::LoginResult AuthService::login(
        const std::string& email,
        const std::string& password,
        uint64_t conn_id) {

    LoginResult result{false, 0, "", "", "", ""};

    auto user = user_repo_.find_by_email(email);
    if (!user.has_value()) {
        result.error = "User not found";
        return result;
    }

    // 验证密码
    std::string hashed = hash_password(password);
    if (hashed != user->password_hash) {
        result.error = "Wrong password";
        return result;
    }

    // 生成 Token
    std::string token = generate_token(user->id);

    // 标记在线
    SessionInfo session;
    session.user_id   = user->id;
    session.nickname  = user->nickname;
    session.token     = token;
    session.conn_id   = conn_id;
    session.login_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    user_mgr_->add_session(session);
    user_repo_.update_status(user->id, 1);

    result.success    = true;
    result.user_id    = user->id;
    result.token      = token;
    result.nickname   = user->nickname;
    result.avatar_url = user->avatar_url;

    LOG_INFO("User logged in: id=%lu, email=%s", user->id, email.c_str());
    return result;
}

// ── 登出 ─────────────────────────────────────────────────
void AuthService::logout(uint64_t user_id, const std::string& token) {
    user_mgr_->remove_session(user_id);
    user_repo_.update_status(user_id, 0);
    LOG_INFO("User logged out: id=%lu", user_id);
}

// ── Token 验证 ───────────────────────────────────────────
std::optional<AuthService::TokenInfo> AuthService::validate_token(
        const std::string& token) {

    // 提取 user_id（token 格式: hex_user_id_random_random）
    auto underscore = token.find('_');
    if (underscore == std::string::npos) return std::nullopt;

    try {
        uint64_t user_id = std::stoull(token.substr(0, underscore), nullptr, 16);

        // 验证会话是否有效
        auto session = user_mgr_->get_session(user_id);
        if (!session || session->token != token) {
            return std::nullopt;
        }

        TokenInfo info;
        info.user_id  = user_id;
        info.nickname = session->nickname;
        return info;

    } catch (...) {
        return std::nullopt;
    }
}

} // namespace wemeet
