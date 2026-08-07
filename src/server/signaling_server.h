#pragma once
#include "event_loop.h"
#include "event_loop_pool.h"
#include "tcp_server.h"
#include "connection_pool.h"
#include "auth_service.h"
#include "meeting_service.h"
#include "user_manager.h"
#include "media_relay.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace wemeet {

/**
 * @brief 信令服务器总控 — 整合所有子系统（包含媒体中继）
 *
 * 扩展功能：
 *   - MediaRelay 媒体中继（SFU）
 *   - 媒体统计与带宽自适应
 *   - 屏幕共享控制
 *   - 参与者角色管理
 */
class SignalingServer {
public:
    struct Config {
        std::string listen_ip       = "0.0.0.0";
        uint16_t    listen_port     = 9090;
        size_t      worker_threads  = 0;

        // 数据库配置
        std::string db_host = "127.0.0.1";
        int         db_port = 3306;
        std::string db_user = "root";
        // #14：禁止硬编码默认密码。默认空，启动时由 main.cpp 校验：
        // 命令行 --db-pass 优先，其次环境变量 WEMEET_DB_PASS，仍为空则报错退出。
        std::string db_pass = "";
        std::string db_name = "wemeet";

        // 媒体中继配置
        std::string relay_ip       = "0.0.0.0";
        uint16_t    relay_base_port = 10000;
        int         relay_threads   = 2;
    };

    explicit SignalingServer(const Config& config);
    ~SignalingServer();

    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    // ── 生命周期 ─────────────────────────────────────────
    void start();
    void stop();

    // ── 消息分发 ─────────────────────────────────────────
    void on_message(const std::shared_ptr<TcpConnection>& conn, Buffer& buf);

private:
    // 原有消息处理
    void handle_auth_by_id(const std::shared_ptr<TcpConnection>& conn,
                            uint64_t seq_id, Buffer& buf);
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

    // 新的媒体相关消息处理
    void handle_media_relay_register(const std::shared_ptr<TcpConnection>& conn,
                                     uint64_t seq_id, Buffer& buf);
    void handle_media_stats_report(const std::shared_ptr<TcpConnection>& conn,
                                   uint64_t seq_id, Buffer& buf);
    void handle_media_control(const std::shared_ptr<TcpConnection>& conn,
                              uint64_t seq_id, Buffer& buf);
    void handle_screen_share_req(const std::shared_ptr<TcpConnection>& conn,
                                 uint64_t seq_id, Buffer& buf);
    void handle_participant_role(const std::shared_ptr<TcpConnection>& conn,
                                 uint64_t seq_id, Buffer& buf);

    // 辅助：通过 user_id 发送消息
    void send_to_user(uint64_t user_id, const void* data, size_t len);
    void broadcast_to_room(const std::string& room_id,
                           uint64_t exclude_user, Buffer&& buf);

    // ── 子系统 ───────────────────────────────────────────
    Config                         config_;
    std::unique_ptr<EventLoop>     main_loop_;
    std::unique_ptr<EventLoopPool> loop_pool_;
    std::unique_ptr<TcpServer>     tcp_server_;
    std::unique_ptr<ConnectionPool> db_pool_;
    std::unique_ptr<AuthService>    auth_service_;
    std::unique_ptr<MeetingService> meeting_service_;
    std::unique_ptr<UserManager>    user_manager_;
    std::unique_ptr<MediaRelay>     media_relay_;

    // user_id → conn_id 映射（用于信令消息路由）
    std::mutex user_conn_mutex_;
    std::unordered_map<uint64_t, uint64_t> user_conn_map_;
};

} // namespace wemeet
