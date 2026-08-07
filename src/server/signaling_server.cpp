#include "signaling_server.h"
#include "codec.h"
#include "logger.h"
#include "common.pb.h"
#include "auth.pb.h"
#include "meeting.pb.h"
#include "signaling.pb.h"
#include "media.pb.h"
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

    // 媒体中继
    media_relay_ = std::make_unique<MediaRelay>(config.relay_ip, config.relay_base_port);

    // TCP 服务器
    tcp_server_ = std::make_unique<TcpServer>(
        main_loop_.get(), loop_pool_.get(), config.listen_ip, config.listen_port);

    tcp_server_->set_message_callback(
        [this](const std::shared_ptr<TcpConnection>& conn, Buffer& buf) {
            on_message(conn, buf);
        });

    tcp_server_->set_connection_callback(
        [this](const std::shared_ptr<TcpConnection>& conn) {
            if (conn->connected()) {
                // 新连接建立
                LOG_INFO("Client connected: conn_id=%lu, fd=%d",
                         conn->conn_id(), conn->socket().fd());
            } else {
                // 连接关闭时清理 user→conn 映射
                if (auto* uid = conn->get_context<uint64_t>()) {
                    std::lock_guard<std::mutex> lock(user_conn_mutex_);
                    user_conn_map_.erase(*uid);
                    LOG_INFO("User %lu disconnected, conn_id=%lu, cleaned up mapping",
                             *uid, conn->conn_id());
                } else {
                    LOG_INFO("Client disconnected: conn_id=%lu (unauthenticated)",
                             conn->conn_id());
                }
            }
        });

    LOG_INFO("SignalingServer initialized on %s:%u, relay on %s:%u+",
             config.listen_ip.c_str(), config.listen_port,
             config.relay_ip.c_str(), config.relay_base_port);
}

SignalingServer::~SignalingServer() {
    stop();
}

void SignalingServer::start() {
    LOG_INFO("=== SignalingServer::start() entered ===");
    loop_pool_->start();
    LOG_INFO("=== loop_pool started ===");
    // 启动媒体中继
    media_relay_->start(config_.relay_threads);
    LOG_INFO("=== media_relay started, calling tcp_server_->start() ===");
    // 注册 TCP 监听 socket 到 epoll，开始接受客户端连接
    if (tcp_server_) {
        LOG_INFO("=== tcp_server_ is valid, calling start ===");
        tcp_server_->start();
        LOG_INFO("=== tcp_server_->start() returned ===");
    } else {
        LOG_WARN("=== tcp_server_ is null! ===");
    }
    LOG_INFO("SignalingServer starting...");
    main_loop_->loop();
}

void SignalingServer::stop() {
    media_relay_->stop();
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
        LOG_WARN("Failed to decode BaseMessage from conn=%lu (buf_size=%zu)",
                 conn->conn_id(), buf.readable_size());
        return;
    }

    uint64_t seq_id = base.sequence_id();
    uint64_t from_user = 0;
    if (auto* uid = conn->get_context<uint64_t>()) from_user = *uid;

    LOG_INFO("RECV conn=%lu user=%llu type=%d seq=%llu payload=%zuB",
             conn->conn_id(), from_user, static_cast<int>(base.type()),
             seq_id, base.payload().size());

    // 注意：buf 此时已被 decode_base_message 消费，handlers 不能再读 buf
    // 各 handler 必须从 base.payload() 自行解码

    switch (base.type()) {
    case MsgType::MSG_LOGIN_REQ: {
        // 在这里直接解码，因为 buf 已被消耗
        LoginReq req;
        if (req.ParseFromString(base.payload())) {
            LOG_INFO("LOGIN_REQ parsed: email=%s", req.email().c_str());
            auto result = auth_service_->login(req.email(), req.password(), conn->conn_id());
            LOG_INFO("login result: success=%d, user_id=%lu, err=%s",
                     result.success, result.user_id, result.error.c_str());

            LoginResp resp;
            resp.set_success(result.success);
            resp.set_user_id(result.user_id);
            resp.set_token(result.token);
            resp.set_nickname(result.nickname);
            resp.set_avatar_url(result.avatar_url);
            resp.set_error_msg(result.error);

            auto resp_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_LOGIN_RESP), seq_id, resp);
            size_t resp_size = resp_buf.readable_size();
            conn->send(std::move(resp_buf));
            LOG_INFO("SEND -> conn=%llu LOGIN_RESP %zuB", conn->conn_id(), resp_size);

            if (result.success) {
                conn->set_context(std::make_any<uint64_t>(result.user_id));
                std::lock_guard<std::mutex> lock(user_conn_mutex_);
                user_conn_map_[result.user_id] = conn->conn_id();
            }
        } else {
            LOG_WARN("Failed to parse LOGIN_REQ payload");
        }
        break;
    }
    case MsgType::MSG_AUTH_BY_ID_REQ: {
        // 同上：直接处理
        LOG_INFO("AUTH_BY_ID_REQ: parsing payload %zu bytes", base.payload().size());
        AuthByIdReq req;
        if (req.ParseFromString(base.payload())) {
            LOG_INFO("AUTH_BY_ID_REQ: user=%lu nick=%s, conn=%llu",
                     req.user_id(), req.nickname().c_str(), conn->conn_id());
            conn->set_context(std::make_any<uint64_t>(req.user_id()));
            std::lock_guard<std::mutex> lock(user_conn_mutex_);
            user_conn_map_[req.user_id()] = conn->conn_id();

            AuthByIdResp resp;
            resp.set_success(true);
            resp.set_user_id(req.user_id());

            auto resp_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_AUTH_BY_ID_RESP), seq_id, resp);
            size_t sz = resp_buf.readable_size();
            conn->send(std::move(resp_buf));
            LOG_INFO("SEND -> conn=%llu AUTH_BY_ID_RESP %zuB", conn->conn_id(), sz);
        } else {
            LOG_WARN("AUTH_BY_ID_REQ: ParseFromString FAILED");
        }
        break;
    }
    case MsgType::MSG_REGISTER_REQ: {
        // 直接处理：buf 已被 decode_base_message 消耗，从 base.payload() 解码
        RegisterReq req;
        if (req.ParseFromString(base.payload())) {
            LOG_INFO("REGISTER_REQ parsed: email=%s, nickname=%s",
                     req.email().c_str(), req.nickname().c_str());
            auto result = auth_service_->register_user(
                req.email(), req.password(), req.nickname());
            LOG_INFO("register result: success=%d, user_id=%lu, err=%s",
                     result.success, result.user_id, result.error.c_str());

            RegisterResp resp;
            resp.set_success(result.success);
            resp.set_user_id(result.user_id);
            resp.set_error_msg(result.error);

            auto resp_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_REGISTER_RESP), seq_id, resp);
            size_t sz = resp_buf.readable_size();
            conn->send(std::move(resp_buf));
            LOG_INFO("SEND -> conn=%llu REGISTER_RESP %zuB", conn->conn_id(), sz);
        } else {
            LOG_WARN("Failed to parse REGISTER_REQ payload");
        }
        break;
    }
    case MsgType::MSG_HEARTBEAT_REQ:
        handle_heartbeat(conn, seq_id, buf);
        break;
    case MsgType::MSG_MEETING_CREATE_REQ: {
        CreateMeetingReq req;
        if (req.ParseFromString(base.payload())) {
            auto result = meeting_service_->create_meeting(
                req.creator_id(), req.title(), req.max_participants());
            CreateMeetingResp resp;
            resp.set_success(result.success);
            resp.set_room_id(result.room_id);
            resp.set_error_msg(result.error);
            auto resp_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_MEETING_CREATE_RESP), seq_id, resp);
            conn->send(std::move(resp_buf));
            LOG_INFO("MEETING_CREATE: creator=%lu room=%s success=%d",
                     req.creator_id(), result.room_id.c_str(), result.success);
        }
        break;
    }
    case MsgType::MSG_MEETING_JOIN_REQ: {
        JoinMeetingReq req;
        if (req.ParseFromString(base.payload())) {
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
            LOG_INFO("MEETING_JOIN: user=%lu nick=%s room=%s success=%d total=%zu",
                     req.user_id(), req.nickname().c_str(), req.room_id().c_str(),
                     result.success, result.participants.size());
            if (result.success) {
                // 通知其他参与者有人加入
                ParticipantUpdate update;
                update.set_room_id(req.room_id());
                update.set_action(ParticipantUpdate::JOINED);
                auto* p = update.mutable_participant();
                p->set_user_id(req.user_id());
                p->set_nickname(req.nickname());
                p->set_audio_on(true);
                p->set_video_on(true);
                p->set_is_host(false);
                auto bcast_buf = Codec::encode_wrapped(
                    static_cast<int>(MsgType::MSG_MEETING_PARTICIPANT), seq_id, update);
                broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));
            }
        }
        break;
    }
    case MsgType::MSG_MEETING_LEAVE: {
        LeaveMeeting req;
        if (req.ParseFromString(base.payload())) {
            meeting_service_->leave_meeting(req.user_id(), req.room_id());
            media_relay_->unregister_participant(req.user_id(), req.room_id());
            // 通知其他人
            ParticipantUpdate update;
            update.set_room_id(req.room_id());
            update.set_action(ParticipantUpdate::LEFT);
            auto* p = update.mutable_participant();
            p->set_user_id(req.user_id());
            auto bcast_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_MEETING_PARTICIPANT), seq_id, update);
            broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));
            // 清除 user_conn_map 映射
            {
                std::lock_guard<std::mutex> lock(user_conn_mutex_);
                auto it = user_conn_map_.find(req.user_id());
                if (it != user_conn_map_.end()) user_conn_map_.erase(it);
            }
            LOG_INFO("MEETING_LEAVE: user=%lu room=%s", req.user_id(), req.room_id().c_str());
        }
        break;
    }
    case MsgType::MSG_CHAT_SEND: {
        ChatSend req;
        if (req.ParseFromString(base.payload())) {
            LOG_INFO("CHAT: user=%lu room=%s content='%s'",
                     req.user_id(), req.room_id().c_str(), req.content().c_str());
            // 从 session 获取 nickname
            std::string nickname;
            auto session = user_manager_->get_session(req.user_id());
            if (session) nickname = session->nickname;
            if (nickname.empty()) nickname = "user_" + std::to_string(req.user_id());
            // 构造 ChatReceive 广播给房间内其他人
            ChatReceive broadcast;
            broadcast.set_user_id(req.user_id());
            broadcast.set_nickname(nickname);
            broadcast.set_room_id(req.room_id());
            broadcast.set_content(req.content());
            broadcast.set_timestamp(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            auto bcast_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_CHAT_RECEIVE), seq_id, broadcast);
            broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));
        }
        break;
    }
    // ── 新增加配媒体消息 ──────────────────────────────
    case MsgType::MSG_MEDIA_RELAY_REGISTER: {
        MediaRelayRegister req;
        if (req.ParseFromString(base.payload())) {
            uint32_t allocated_ssrc = 0;
            std::string relay_host;
            uint16_t relay_port = 0;
            bool ok = media_relay_->register_participant(
                req.user_id(), req.room_id(), "user",
                req.relay_host(), req.relay_port(),
                req.media_type(), allocated_ssrc, relay_host, relay_port);
            MediaRelayRegisterResp resp;
            resp.set_success(ok);
            resp.set_allocated_host(relay_host);
            resp.set_allocated_port(relay_port);
            resp.set_ssrc(allocated_ssrc);
            // 添加房间内其他参与者
            auto participants = media_relay_->get_room_participants(req.room_id());
            for (auto& p : participants) {
                if (p.user_id == req.user_id()) continue;
                auto* peer = resp.add_peers();
                peer->set_user_id(p.user_id);
                peer->set_nickname(p.nickname);
                for (auto& [mt, si] : p.streams) {
                    peer->set_ssrc(si->ssrc);
                }
                peer->set_audio_on(p.audio_on);
                peer->set_video_on(p.video_on);
                peer->set_screen_share(p.screen_sharing);
            }
            auto resp_buf = Codec::encode_wrapped(
                static_cast<int>(MsgType::MSG_MEDIA_RELAY_REGISTER_RESP), seq_id, resp);
            conn->send(std::move(resp_buf));
            LOG_INFO("MediaRelay: user=%lu room=%s type=%s ssrc=%u ok=%d peers=%d",
                     req.user_id(), req.room_id().c_str(), req.media_type().c_str(),
                     allocated_ssrc, ok ? 1 : 0, resp.peers_size());
        }
        break;
    }
    case MsgType::MSG_MEDIA_STATS_REPORT:
        handle_media_stats_report(conn, seq_id, buf);
        break;
    case MsgType::MSG_MEDIA_CONTROL:
        handle_media_control(conn, seq_id, buf);
        break;
    case MsgType::MSG_SCREEN_SHARE_REQ:
        handle_screen_share_req(conn, seq_id, buf);
        break;
    case MsgType::MSG_PARTICIPANT_ROLE:
        handle_participant_role(conn, seq_id, buf);
        break;
    default:
        LOG_WARN("Unknown message type: %d", static_cast<int>(base.type()));
        break;
    }
}

// ── 已有处理器（简化，保持原有逻辑）────────────────────

void SignalingServer::handle_auth_by_id(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage base;
    AuthByIdReq req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 直接注册到 user_conn_map_
    conn->set_context(std::make_any<uint64_t>(req.user_id()));
    {
        std::lock_guard<std::mutex> lock(user_conn_mutex_);
        user_conn_map_[req.user_id()] = conn->conn_id();
    }

    AuthByIdResp resp;
    resp.set_success(true);
    resp.set_user_id(req.user_id());

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_AUTH_BY_ID_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));

    LOG_INFO("Auth by ID: user=%lu (%s) registered, conn_id=%lu",
             req.user_id(), req.nickname().c_str(), conn->conn_id());
}

void SignalingServer::handle_login(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {
    // buf 在 on_message 中已被 decode_base_message 消费，
    // 需用另一种方式获取 payload
    // 简化：直接调用 auth_service（使用占位）
    std::string email, password;
    // 重新构造：先 peek 一下 buffer 剩余内容（实际上已经空了）
    // 真实场景需要重构 on_message，把已解码的 BaseMessage 传下来
    // 这里用 last_user_email 临时传递

    // 临时方案：从 conn 的扩展上下文读取（由 on_message 暂存）
    std::string* cached = conn->get_context<std::string>();
    if (cached) {
        size_t sep = cached->find('\n');
        if (sep != std::string::npos) {
            email = cached->substr(0, sep);
            password = cached->substr(sep + 1);
        }
    }
    if (email.empty() && password.empty()) {
        // 退化：空凭据
        email = "";
        password = "";
    }

    auto result = auth_service_->login(email, password, conn->conn_id());
    LOG_INFO("handle_login: email=%s, success=%d, user_id=%lu, err=%s",
             email.c_str(), result.success, result.user_id, result.error.c_str());

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
    LOG_INFO("SEND -> conn=%llu LOGIN_RESP %zuB", conn->conn_id(), resp_buf.readable_size());

    if (result.success) {
        conn->set_context(std::make_any<uint64_t>(result.user_id));
        {
            std::lock_guard<std::mutex> lock(user_conn_mutex_);
            user_conn_map_[result.user_id] = conn->conn_id();
        }
    }
}

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

    LOG_INFO("User %lu (%s) joined room %s, total=%zu participants",
             req.user_id(), req.nickname().c_str(), req.room_id().c_str(),
             result.participants.size());

    // 通知其他参与者
    if (result.success) {
        ParticipantUpdate update;
        update.set_room_id(req.room_id());
        update.set_action(ParticipantUpdate::JOINED);
        auto* p = update.mutable_participant();
        p->set_user_id(req.user_id());
        p->set_nickname(req.nickname());
        p->set_audio_on(true);
        p->set_video_on(true);
        p->set_is_host(false);

        auto bcast_buf = Codec::encode_wrapped(
            static_cast<int>(MsgType::MSG_MEETING_PARTICIPANT), seq_id, update);
        broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));
    }
}

void SignalingServer::handle_leave_meeting(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t /*seq_id*/, Buffer& buf) {
    BaseMessage  base;
    LeaveMeeting req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    meeting_service_->leave_meeting(req.user_id(), req.room_id());
    media_relay_->unregister_participant(req.user_id(), req.room_id());

    // 通知其他参与者
    ParticipantUpdate update;
    update.set_room_id(req.room_id());
    update.set_action(ParticipantUpdate::LEFT);
    auto* p = update.mutable_participant();
    p->set_user_id(req.user_id());

    auto bcast_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEETING_PARTICIPANT), 0, update);
    broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));

    LOG_INFO("User %lu left room %s", req.user_id(), req.room_id().c_str());
}

void SignalingServer::handle_sdp_offer(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {
    BaseMessage base;
    SDPOffer    req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_SDP_OFFER), seq_id, req);
    send_to_user(req.to_user_id(), fwd_buf.data(), fwd_buf.readable_size());
}

void SignalingServer::handle_sdp_answer(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {
    BaseMessage base;
    SDPAnswer   req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_SDP_ANSWER), seq_id, req);
    send_to_user(req.to_user_id(), fwd_buf.data(), fwd_buf.readable_size());
}

void SignalingServer::handle_ice_candidate(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {
    BaseMessage  base;
    ICECandidate req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_ICE_CANDIDATE), seq_id, req);
    send_to_user(req.to_user_id(), fwd_buf.data(), fwd_buf.readable_size());
}

void SignalingServer::handle_chat(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {
    BaseMessage base;
    ChatSend    req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    ChatReceive broadcast;
    broadcast.set_user_id(req.user_id());
    broadcast.set_room_id(req.room_id());
    broadcast.set_content(req.content());
    broadcast.set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto bcast_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_CHAT_RECEIVE), seq_id, broadcast);
    broadcast_to_room(req.room_id(), req.user_id(), std::move(bcast_buf));
}

// ── 新增媒体消息处理器 ────────────────────────────────────

void SignalingServer::handle_media_relay_register(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage       base;
    MediaRelayRegister req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    MediaRelayRegisterResp resp;
    uint32_t allocated_ssrc = 0;
    std::string relay_host;
    uint16_t relay_port = 0;

    bool ok = media_relay_->register_participant(
        req.user_id(), req.room_id(), "user",
        req.relay_host(), req.relay_port(),
        req.media_type(), allocated_ssrc, relay_host, relay_port);

    resp.set_success(ok);
    resp.set_allocated_host(relay_host);
    resp.set_allocated_port(relay_port);
    resp.set_ssrc(allocated_ssrc);

    // 添加房间内其他参与者信息
    auto participants = media_relay_->get_room_participants(req.room_id());
    for (auto& p : participants) {
        if (p.user_id == req.user_id()) continue;
        auto* peer = resp.add_peers();
        peer->set_user_id(p.user_id);
        peer->set_nickname(p.nickname);
        for (auto& [mt, si] : p.streams) {
            peer->set_ssrc(si->ssrc);
        }
        peer->set_audio_on(p.audio_on);
        peer->set_video_on(p.video_on);
        peer->set_screen_share(p.screen_sharing);
    }

    auto resp_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEDIA_RELAY_REGISTER_RESP), seq_id, resp);
    conn->send(std::move(resp_buf));

    LOG_INFO("Media relay registered: user=%lu, room=%s, type=%s, ssrc=%u",
             req.user_id(), req.room_id().c_str(), req.media_type().c_str(), allocated_ssrc);
}

void SignalingServer::handle_media_stats_report(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t /*seq_id*/, Buffer& buf) {

    BaseMessage      base;
    MediaStatsReport req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    media_relay_->report_stats(req.user_id(), req.room_id(),
                                req.packet_loss_rate(), req.round_trip_time(),
                                req.jitter_ms(), req.bitrate_kbps(),
                                req.signal_quality());

    // 自适应带宽: 检测是否需要发送带宽提示
    uint32_t suggested = media_relay_->suggest_bitrate(req.user_id(), req.room_id());

    // 如果需要调整，发送 BandwidthHint
    BandwidthHint hint;
    hint.set_user_id(req.user_id());
    hint.set_max_bitrate_kbps(suggested);
    hint.set_min_bitrate_kbps(suggested / 3);
    hint.set_reason(suggested < 1000 ? "congestion" : "normal");

    auto hint_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_BANDWIDTH_HINT), 0, hint);
    send_to_user(req.user_id(), hint_buf.data(), hint_buf.readable_size());

    LOG_DEBUG("Media stats from %lu: loss=%.2f%%, rtt=%.1fms, suggested=%ukbps",
              req.user_id(), req.packet_loss_rate() * 100, req.round_trip_time(), suggested);
}

void SignalingServer::handle_media_control(
        const std::shared_ptr<TcpConnection>& conn,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage  base;
    MediaControl req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 转给目标用户
    MediaControl fwd;
    fwd.set_target_user_id(req.target_user_id());
    fwd.set_room_id(req.room_id());
    fwd.set_media_type(req.media_type());
    fwd.set_mute(req.mute());

    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEDIA_CONTROL), seq_id, fwd);
    send_to_user(req.target_user_id(), fwd_buf.data(), fwd_buf.readable_size());

    // 更新媒体中继状态
    if (req.media_type() == MediaControl::AUDIO) {
        media_relay_->update_media_status(req.target_user_id(), req.room_id(),
                                           !req.mute(), true);
    } else if (req.media_type() == MediaControl::VIDEO) {
        media_relay_->update_media_status(req.target_user_id(), req.room_id(),
                                           true, !req.mute());
    } else if (req.media_type() == MediaControl::SCREEN) {
        // 屏幕共享冻结标志（#20）：mute=true=暂停共享，false=恢复。
        // 除回执给操作者外，还需广播给房间内其他参与者（排除发送者），
        // 让所有观看端叠加/解除「对方已暂停共享」提示。
        auto bcast_buf = Codec::encode_wrapped(
            static_cast<int>(MsgType::MSG_MEDIA_CONTROL), seq_id, fwd);
        broadcast_to_room(req.room_id(), req.target_user_id(), std::move(bcast_buf));

        LOG_INFO("Screen share %s broadcast: user=%lu room=%s",
                 req.mute() ? "PAUSED" : "RESUMED",
                 req.target_user_id(), req.room_id().c_str());
    }

    MediaControlAck ack;
    ack.set_user_id(req.target_user_id());
    ack.set_room_id(req.room_id());
    ack.set_success(true);

    auto ack_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_MEDIA_CONTROL_ACK), seq_id, ack);
    conn->send(std::move(ack_buf));
}

void SignalingServer::handle_screen_share_req(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage    base;
    ScreenShareReq req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 广播屏幕共享状态给房间内所有人
    ScreenShareNotify notify;
    notify.set_user_id(req.user_id());
    notify.set_room_id(req.room_id());
    notify.set_started(req.start_share());
    // nickname 从 meeting_service 获取
    auto room = meeting_service_->get_room(req.room_id());
    if (room) {
        for (auto& p : room->participants) {
            if (p.user_id == req.user_id()) {
                notify.set_nickname(p.nickname);
                break;
            }
        }
    }

    auto bcast_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_SCREEN_SHARE_NOTIFY), seq_id, notify);
    broadcast_to_room(req.room_id(), 0, std::move(bcast_buf));

    LOG_INFO("Screen share: user=%lu, room=%s, %s",
             req.user_id(), req.room_id().c_str(),
             req.start_share() ? "started" : "stopped");
}

void SignalingServer::handle_participant_role(
        const std::shared_ptr<TcpConnection>& /*conn*/,
        uint64_t seq_id, Buffer& buf) {

    BaseMessage      base;
    ParticipantRole  req;
    Codec::decode_base_message(buf, base);
    Codec::decode_payload(base, req);

    // 转发角色变更
    auto fwd_buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_PARTICIPANT_ROLE), seq_id, req);
    broadcast_to_room(req.room_id(), 0, std::move(fwd_buf));

    LOG_INFO("Role changed: user=%lu in room=%s to role=%d",
             req.user_id(), req.room_id().c_str(), static_cast<int>(req.role()));
}

// ── 消息路由辅助 ───────────────────────────────────────────

void SignalingServer::send_to_user(uint64_t user_id, const void* data, size_t len) {
    uint64_t conn_id = 0;
    {
        std::lock_guard<std::mutex> lock(user_conn_mutex_);
        auto it = user_conn_map_.find(user_id);
        if (it == user_conn_map_.end()) {
            LOG_WARN("SEND-FAIL user=%llu not in user_conn_map (size=%zu)",
                     user_id, user_conn_map_.size());
            return;
        }
        conn_id = it->second;
    }

    // 精确定位到指定 conn_id 的连接
    if (tcp_server_) {
        LOG_INFO("SEND -> user=%llu conn=%llu size=%zuB", user_id, conn_id, len);
        tcp_server_->send_to_conn(conn_id, data, len);
    } else {
        LOG_WARN("SEND-FAIL no tcp_server_");
    }
}

void SignalingServer::broadcast_to_room(
        const std::string& room_id, uint64_t exclude_user,
        Buffer&& buf) {

    auto room = meeting_service_->get_room(room_id);
    if (!room) {
        LOG_WARN("BROADCAST-FAIL room=%s not found", room_id.c_str());
        return;
    }

    LOG_INFO("BROADCAST room=%s total=%zu exclude_user=%llu size=%zuB",
             room_id.c_str(), room->participants.size(), exclude_user,
             buf.readable_size());

    for (auto& p : room->participants) {
        if (p.user_id == exclude_user) continue;
        send_to_user(p.user_id, buf.data(), buf.readable_size());
    }
}

} // namespace wemeet
