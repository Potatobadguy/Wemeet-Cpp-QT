#include "signaling_server.h"
#include "codec.h"
#include "logger.h"
#include "common.pb.h"
#include "auth.pb.h"
#include "meeting.pb.h"
#include "signaling.pb.h"
#include <cstring>

namespace wemeet {

SignalingServer::SignalingServer(const Config& config)
    : config_(config) {

    main_loop_  = std::make_unique<EventLoop>();
    loop_pool_  = std::make_unique<EventLoopPool>(config.worker_threads);
    user_manager_ = std::make_unique<UserManager>();

    // 数据库连接池
    ConnectionPool::Config db_config;
    db_config.host     = config.db_host;
    db_config.port     = config.db_port;
    db_config.user     = config.db_user;
    db_config.password = config.db_pass;
    db_config.database = config.db_name;
    db_pool_ = std::make_unique<ConnectionPool>(db_config);

    // 业务服务
    auth_service_    = std::make_unique<AuthService>(db_pool_.get(), user_manager_.get());
    meeting_service_ = std::make_unique<MeetingService>(db_pool_.get());

    // TCP 服务器
    tcp_server_ = std::make_unique<TcpServer>(
        main_loop_.get(), loop_pool_.get(), config.listen_ip, config.listen_port);

    tcp_server_->set_message_callback(
        [this](const std::shared_ptr<TcpConnection>& conn, Buffer& buf) {
            on_message(conn, buf);
        });

    LOG_INFO("SignalingServer initialized on %s:%u",
             config.listen_ip.c_str(), config.listen_port);
}

SignalingServer::~SignalingServer() {
    stop();
}

void SignalingServer::start() {
    loop_pool_->start();

    // 主循环处理 accept
    LOG_INFO("SignalingServer starting...");
    main_loop_->loop();
}

void SignalingServer::stop() {
    loop_pool_->stop();
    main_loop_->quit();
    if (tcp_server_) tcp_server_->stop();
    LOG_INFO("SignalingServer stopped");
}

// ── 消息分发 ─────────────────────────────────────────────
void SignalingServer::on_message(
        const std::shared_ptr<TcpConnection>& conn, Buffer& buf) {

    BaseMessage base;
    if (!Codec::decode_base_message(buf, base)) {
        LOG_WARN("Failed to decode BaseMessage from conn=%lu", conn->conn_id());
        return;
    }

    uint64_t seq_id = base.sequence_id();

    switch (base.type()) {
    case MsgType::MSG_LOGIN_REQ:
        handle_login(conn, seq_id, buf);
        break;
    case MsgType::MSG_REGISTER_REQ:
        handle_register(conn, seq_id, buf);
        break;
    case MsgType::MSG_HEARTBEAT_REQ:
        handle_heartbeat(conn, seq_id, buf);
        break;
    case MsgType::MSG_MEETING_CREATE_REQ:
        handle_create_meeting(conn, seq_id, buf);
        break;
    case MsgType::MSG_MEETING_JOIN_REQ:
        handle_join_meeting(conn, seq_id, buf);
        break;
    case MsgType::MSG_MEETING_LEAVE:
        handle_leave_meeting(conn, seq_id, buf);
        break;
    case MsgType::MSG_SDP_OFFER:
        handle_sdp_offer(conn, seq_id, buf);
        break;
    case MsgType::MSG_SDP_ANSWER:
        handle_sdp_answer(conn, seq_id, buf);
        break;
    case MsgType::MSG_ICE_CANDIDATE:
        handle_ice_candidate(conn, seq_id, buf);
        break;
    case MsgType::MSG_CHAT_SEND:
        handle_chat(conn, seq_id, buf);
        break;
    default:
        LOG_WARN("Unknown message type: %d", static_cast<int>(base.type()));
        break;
    }
}

// ── 登录处理 ─────────────────────────────────────────────
void SignalingServer::handle_login(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage base;
    LoginReq    req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto result = auth_service_->login(req.email(), req.password(), conn->conn_id());

    LoginResp resp;
    resp.set_success(result.success);
    resp.set_user_id(result.user_id);
    resp.set_token(result.token);
    resp.set_nickname(result.nickname);
    resp.set_avatar_url(result.avatar_url);
    resp.set_error_msg(result.error);

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_LOGIN_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));
}

// ── 注册处理 ─────────────────────────────────────────────
void SignalingServer::handle_register(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage  base;
    RegisterReq  req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto result = auth_service_->register_user(req.email(), req.password(), req.nickname());

    RegisterResp resp;
    resp.set_success(result.success);
    resp.set_user_id(result.user_id);
    resp.set_error_msg(result.error);

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_REGISTER_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));
}

// ── 心跳处理 ─────────────────────────────────────────────
void SignalingServer::handle_heartbeat(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& /*buf*/) {

    HeartBeatResp resp;
    resp.set_server_time_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_HEARTBEAT_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));
}

// ── 创建会议 ─────────────────────────────────────────────
void SignalingServer::handle_create_meeting(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage     base;
    CreateMeetingReq req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto result = meeting_service_->create_meeting(
        req.creator_id(), req.title(), req.max_participants());

    CreateMeetingResp resp;
    resp.set_success(result.success);
    resp.set_room_id(result.room_id);
    resp.set_error_msg(result.error);

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEETING_CREATE_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));
}

// ── 加入会议 ─────────────────────────────────────────────
void SignalingServer::handle_join_meeting(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage    base;
    JoinMeetingReq req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto result = meeting_service_->join_meeting(
        req.user_id(), req.nickname(), req.room_id());

    JoinMeetingResp resp;
    resp.set_success(result.success);
    resp.set_error_msg(result.error);
    for (auto& p : result.participants) {
        auto* pb_p = resp.add_participants();
        pb_p->set_user_id(p.user_id);
        pb_p->set_nickname(p.nickname);
        pb_p->set_audio_on(p.audio_on);
        pb_p->set_video_on(p.video_on);
        pb_p->set_is_host(p.is_host);
    }

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEETING_JOIN_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));

    // 通知其他参与者
    if (result.success) {
        ParticipantUpdate update;
        update.set_room_id(req.room_id());
        update.set_action(ParticipantUpdate::JOINED);
        // 广播...
    }
}

// ── 离开会议 ─────────────────────────────────────────────
void SignalingServer::handle_leave_meeting(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t /*seq_id*/, Buffer& buf) {

    BaseMessage  base;
    LeaveMeeting req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    meeting_service_->leave_meeting(req.user_id(), req.room_id());
}

// ── SDP 转发 ─────────────────────────────────────────────
void SignalingServer::handle_sdp_offer(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage base;
    SDPOffer    req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 转发 SDP Offer 给目标用户
    SDPOffer fwd;
    fwd.set_from_user_id(req.from_user_id());
    fwd.set_to_user_id(req.to_user_id());
    fwd.set_room_id(req.room_id());
    fwd.set_sdp(req.sdp());
    fwd.set_type("offer");

    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_SDP_OFFER), seq_id, fwd);
    // tcp_server_ 需要能根据 user_id 找到连接来发送
    LOG_DEBUG("SDP Offer: from=%lu to=%lu", req.from_user_id(), req.to_user_id());
}

void SignalingServer::handle_sdp_answer(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage base;
    SDPAnswer   req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    LOG_DEBUG("SDP Answer: from=%lu to=%lu", req.from_user_id(), req.to_user_id());
}

void SignalingServer::handle_ice_candidate(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t /*seq_id*/, Buffer& buf) {

    BaseMessage  base;
    ICECandidate req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    LOG_DEBUG("ICE Candidate: from=%lu to=%lu", req.from_user_id(), req.to_user_id());
}

// ── 聊天 ─────────────────────────────────────────────────
void SignalingServer::handle_chat(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage base;
    ChatSend    req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 广播给房间内其他人
    ChatReceive broadcast;
    broadcast.set_user_id(req.user_id());
    broadcast.set_room_id(req.room_id());
    broadcast.set_content(req.content());
    broadcast.set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto bcast_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_CHAT_RECEIVE), seq_id, broadcast);
    // tcp_server_->broadcast(...)
    LOG_DEBUG("Chat: user=%lu, room=%s", req.user_id(), req.room_id().c_str());
}

} // namespace wemeet
