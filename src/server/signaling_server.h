#pragma once
#include "event_loop.h"
#include "event_loop_pool.h"
#include "tcp_server.h"
#include "connection_pool.h"
#include "auth_service.h"
#include "meeting_service.h"
#include "user_manager.h"
#include <memory>
#include <string>

namespace wemeet {

/**
 * @brief 信令服务器总控 — 整合所有子系统
 *
 * 模块集成：
 *   - Reactor + EventLoopPool 网络层
 *   - TcpServer 管理客户端连接
 *   - AuthService 认证
 *   - MeetingService 会议管理
 *   - UserManager 在线用户管理
 */
class SignalingServer {
public:
    struct Config {
        std::string listen_ip   = "0.0.0.0";
        uint16_t    listen_port = 9090;
        size_t      worker_threads = 0;  // 0 = auto (CPU 核数)

        // 数据库配置
        std::string db_host = "127.0.0.1";
        int         db_port = 3306;
        std::string db_user = "root";
        std::string db_pass = "";
        std::string db_name = "wemeet";
    };

    explicit SignalingServer(const Config& config);
    ~SignalingServer();

    // 不可拷贝
    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    // ── 生命周期 ─────────────────────────────────────────
    void start();
    void stop();

    // ── 消息分发 ─────────────────────────────────────────
    void on_message(const std::shared_ptr<TcpConnection>& conn, Buffer& buf);

private:
    // 各消息类型处理
    void handle_login(const std::shared_ptr<TcpConnection>& conn,
                      uint64_t seq_id, Buffer& buf);
    void handle_register(const std::shared_ptr<TcpConnection>& conn,
                         uint64_t seq_id, Buffer& buf);
    void handle_heartbeat(const std::shared_ptr<TcpConnection>& conn,
                          uint64_t seq_id, Buffer& buf);
    void handle_create_meeting(const std::shared_ptr<TcpConnection>& conn,
                               uint64_t seq_id, Buffer& buf);
    void handle_join_meeting(const std::shared_ptr<TcpConnection>& conn,
                             uint64_t seq_id, Buffer& buf);
    void handle_leave_meeting(const std::shared_ptr<TcpConnection>& conn,
                              uint64_t seq_id, Buffer& buf);
    void handle_sdp_offer(const std::shared_ptr<TcpConnection>& conn,
                          uint64_t seq_id, Buffer& buf);
    void handle_sdp_answer(const std::shared_ptr<TcpConnection>& conn,
                           uint64_t seq_id, Buffer& buf);
    void handle_ice_candidate(const std::shared_ptr<TcpConnection>& conn,
                              uint64_t seq_id, Buffer& buf);
    void handle_chat(const std::shared_ptr<TcpConnection>& conn,
                     uint64_t seq_id, Buffer& buf);

    // ── 子系统 ───────────────────────────────────────────
    Config                      config_;
    std::unique_ptr<EventLoop>  main_loop_;
    std::unique_ptr<EventLoopPool> loop_pool_;
    std::unique_ptr<TcpServer>  tcp_server_;
    std::unique_ptr<ConnectionPool> db_pool_;
    std::unique_ptr<AuthService>    auth_service_;
    std::unique_ptr<MeetingService> meeting_service_;
    std::unique_ptr<UserManager>    user_manager_;
};

} // namespace wemeet
