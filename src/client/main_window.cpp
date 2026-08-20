#include "main_window.h"
#include "meeting_room.h"
#include "network_client.h"
#include "media_engine.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QStatusBar>
#include <QDateTime>
#include <QTimer>

// Protobuf 消息
#include "common.pb.h"
#include "auth.pb.h"
#include "meeting.pb.h"
#include "signaling.pb.h"
#include "media.pb.h"

namespace {

// 辅助：序列化 Protobuf 子消息为 BaseMessage（不添加长度头，长度头由 NetworkClient 添加）
std::string encode_wrapped(int msg_type, uint64_t seq_id, const google::protobuf::Message& payload) {
    std::string payload_bytes;
    payload.SerializeToString(&payload_bytes);

    wemeet::BaseMessage base;
    base.set_type(static_cast<wemeet::MsgType>(msg_type));
    base.set_sequence_id(seq_id);
    base.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    base.set_payload(payload_bytes);

    // 直接返回 BaseMessage 的序列化结果，4 字节长度头由 NetworkClient::send_message 添加
    std::string base_bytes;
    base.SerializeToString(&base_bytes);
    return base_bytes;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , network_(new NetworkClient(this))
    , media_engine_(new MediaEngine(this)) {

    setup_navigation();
    setup_ui();

    stack_->setCurrentIndex(PAGE_LOBBY);
    update_back_button();

    // 连接网络事件
    connect(network_, &NetworkClient::connected,
            this, &MainWindow::on_network_connected);
    connect(network_, &NetworkClient::disconnected,
            this, &MainWindow::on_network_disconnected);
    connect(network_, &NetworkClient::error_occurred,
            this, &MainWindow::on_network_error);
    connect(network_, &NetworkClient::message_received,
            this, &MainWindow::on_network_message);

    // 连接媒体引擎事件
    connect(media_engine_, &MediaEngine::network_quality_changed,
            this, &MainWindow::on_network_quality_changed);

    // 媒体引擎通过信令通道向服务器发送统计/控制消息
    connect(media_engine_, &MediaEngine::signaling_message,
            this, [this](const QByteArray& data) {
        if (network_ && network_->is_connected()) {
            network_->send_message(std::string(data.constData(),
                                               static_cast<size_t>(data.size())));
        }
    });

    // 初始状态
    server_status_label_->setText("● 未连接");
    server_status_label_->setStyleSheet(
        "color: #888; font-size: 12px; padding: 4px 8px;");
}

MainWindow::~MainWindow() {
    if (media_engine_) media_engine_->shutdown();
    if (network_) network_->disconnect();
}

void MainWindow::setup_ui() {
    QWidget* central = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    root_layout->addWidget(nav_bar_, 0);
    stack_ = new QStackedWidget(this);
    root_layout->addWidget(stack_, 1);

    setCentralWidget(central);

    // 大厅页
    lobby_page_ = new QWidget(this);
    auto* lobby_layout = new QVBoxLayout(lobby_page_);
    lobby_layout->setAlignment(Qt::AlignCenter);

    auto* welcome_label = new QLabel("欢迎使用 WeMeet");
    welcome_label->setStyleSheet("color: #333; font-size: 28px; font-weight: bold; padding: 20px;");
    welcome_label->setAlignment(Qt::AlignCenter);

    auto* subtitle_label = new QLabel("企业级高清视频会议系统");
    subtitle_label->setStyleSheet("color: #666; font-size: 14px; padding: 0 0 30px 0;");
    subtitle_label->setAlignment(Qt::AlignCenter);

    QString btn_style =
        "QPushButton { color: white; padding: 14px 36px; min-height: 48px; "
        "border-radius: 10px; font-size: 15px; font-weight: bold; min-width: 200px; }";

    create_meeting_btn_ = new QPushButton("[+] 创建会议");
    create_meeting_btn_->setStyleSheet(
        btn_style + "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #4A90D9, stop:1 #357ABD); }");

    auto* join_meeting_btn = new QPushButton("[->] 加入会议");
    join_meeting_btn->setStyleSheet(
        btn_style + "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #50C878, stop:1 #3DA85C); }");

    lobby_layout->addStretch(2);
    lobby_layout->addWidget(welcome_label);
    lobby_layout->addWidget(subtitle_label);
    lobby_layout->addSpacing(30);
    lobby_layout->addWidget(create_meeting_btn_, 0, Qt::AlignCenter);
    lobby_layout->addSpacing(16);
    lobby_layout->addWidget(join_meeting_btn, 0, Qt::AlignCenter);
    lobby_layout->addStretch(3);

    stack_->addWidget(lobby_page_);

    // 会议页
    meeting_page_ = new MeetingRoom(this);
    stack_->addWidget(meeting_page_);

    // 转发会议室的聊天/角色变更信号
    connect(meeting_page_, &MeetingRoom::back_to_lobby,
            this, &MainWindow::on_back_requested);
    connect(meeting_page_, &MeetingRoom::leave_meeting,
            this, &MainWindow::on_logout);
    connect(meeting_page_, &MeetingRoom::send_chat_text,
            this, &MainWindow::send_chat);
    connect(meeting_page_, &MeetingRoom::send_leave_meeting,
            this, [this]() {
        if (in_meeting_) {
            send_leave_meeting();
            in_meeting_ = false;
        }
    });

    connect(create_meeting_btn_, &QPushButton::clicked, this, &MainWindow::on_create_meeting);
    connect(join_meeting_btn, &QPushButton::clicked, this, [this]() {
        bool ok;
        QString room_id = QInputDialog::getText(this, "加入会议",
            "输入房间号:", QLineEdit::Normal, "", &ok);
        if (ok && !room_id.isEmpty()) {
            on_join_meeting(room_id);
        }
    });
}

void MainWindow::setup_navigation() {
    nav_bar_ = new QWidget(this);
    auto* nav_layout = new QHBoxLayout(nav_bar_);
    nav_layout->setContentsMargins(12, 8, 12, 8);
    nav_layout->setSpacing(12);

    back_btn_ = new QPushButton("<-  返回");
    back_btn_->setCursor(Qt::PointingHandCursor);
    back_btn_->setStyleSheet(
        "QPushButton { background: transparent; color: #4A90D9; "
        "border: 1px solid #4A90D9; padding: 6px 16px; border-radius: 6px; "
        "font-size: 14px; font-weight: bold; }"
        "QPushButton:hover { background: #4A90D9; color: white; }"
        "QPushButton:disabled { color: #ccc; border-color: #ddd; }");
    back_btn_->hide();

    user_label_ = new QLabel("WeMeet");
    user_label_->setStyleSheet(
        "color: #333333; font-size: 16px; font-weight: bold; background: transparent;");

    server_status_label_ = new QLabel();
    server_status_label_->setStyleSheet("font-size: 12px; padding: 4px 8px;");

    logout_btn_ = new QPushButton("退出");
    logout_btn_->setStyleSheet(
        "QPushButton { background: transparent; color: #FF6B6B; border: 1px solid #FF6B6B; "
        "padding: 6px 16px; border-radius: 4px; }"
        "QPushButton:hover { background: #FF6B6B; color: white; }");

    nav_layout->addWidget(back_btn_);
    nav_layout->addWidget(back_btn_);
    nav_layout->addWidget(user_label_);
    nav_layout->addStretch();
    nav_layout->addWidget(server_status_label_);
    nav_layout->addWidget(logout_btn_);

    nav_bar_->setStyleSheet("background: #ffffff; border-bottom: 1px solid #e8edf5;");
    nav_bar_->setStyleSheet("background: #ffffff; border-bottom: 1px solid #e8edf5;");
    nav_bar_->setFixedHeight(48);
    nav_bar_->hide();

    connect(back_btn_,   &QPushButton::clicked, this, &MainWindow::on_back_requested);
    connect(back_btn_,   &QPushButton::clicked, this, &MainWindow::on_back_requested);
    connect(logout_btn_, &QPushButton::clicked, this, &MainWindow::on_logout);
}

void MainWindow::update_back_button() {
    bool has_history = !nav_history_.isEmpty();
    bool on_lobby = (stack_->currentIndex() == PAGE_LOBBY);
    back_btn_->setVisible(has_history || !on_lobby);
    back_btn_->setEnabled(has_history || !on_lobby);
}

void MainWindow::navigate_to(int page) {
    nav_history_.push(stack_->currentIndex());
    stack_->setCurrentIndex(page);
    update_back_button();
}

void MainWindow::go_back() {
    if (!nav_history_.isEmpty()) {
        int prev = nav_history_.pop();
        stack_->setCurrentIndex(prev);
    } else {
        stack_->setCurrentIndex(PAGE_LOBBY);
    }
    update_back_button();
}

void MainWindow::on_back_requested() {
    // 关闭媒体引擎（shutdown 内部有 initialized_ 保护，可重复调用）
    if (media_engine_) {
        media_engine_->shutdown();
    }
    // 离开会议时通知服务器
    if (in_meeting_) {
        send_leave_meeting();
        in_meeting_ = false;
    }
    current_room_id_.clear();
    // 确保回到大厅页面（会议选择界面）
    stack_->setCurrentIndex(PAGE_LOBBY);
    nav_history_.clear();
    update_back_button();
}

// ── 服务器连接 ─────────────────────────────────────────────

bool MainWindow::connect_to_server() {
    if (network_->is_connected()) return true;
    qDebug("MainWindow: connecting to %s:%u...", qPrintable(server_host_), server_port_);
    network_->connect_to_server(server_host_, server_port_);

    // 等待连接成功（最多 5 秒，非阻塞事件循环）
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(network_, &NetworkClient::connected, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();

    bool ok = network_->is_connected();
    if (ok) {
        qDebug("MainWindow: connected OK");
    } else {
        qWarning("MainWindow: connect timeout");
    }
    return ok;
}

void MainWindow::login_via_server(const QString& email, const QString& password) {
    if (!network_->is_connected()) {
        emit auth_failed("未连接到服务器，请重启客户端");
        return;
    }

    // 标记等待登录响应
    waiting_login_ = true;
    login_success_ = false;

    wemeet::LoginReq req;
    req.set_email(email.toStdString());
    req.set_password(password.toStdString());

    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_LOGIN_REQ),
        QDateTime::currentMSecsSinceEpoch(), req));

    qDebug("MainWindow: LOGIN_REQ sent for %s", qPrintable(email));

    // 启动超时定时器
    QTimer::singleShot(8000, this, [this]() {
        if (waiting_login_) {
            waiting_login_ = false;
            emit auth_failed("服务器验证超时（8秒）");
        }
    });
}

void MainWindow::register_via_server(const QString& email, const QString& password,
                                     const QString& nickname) {
    if (!network_->is_connected()) {
        emit register_result(false, "未连接到服务器，请重启客户端");
        return;
    }

    waiting_register_ = true;

    wemeet::RegisterReq req;
    req.set_email(email.toStdString());
    req.set_password(password.toStdString());
    req.set_nickname(nickname.toStdString());

    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_REGISTER_REQ),
        QDateTime::currentMSecsSinceEpoch(), req));

    qDebug("MainWindow: REGISTER_REQ sent for %s", qPrintable(email));

    // 启动超时定时器
    QTimer::singleShot(8000, this, [this]() {
        if (waiting_register_) {
            waiting_register_ = false;
            emit register_result(false, "服务器无响应（8秒）");
        }
    });
}

void MainWindow::on_network_connected() {
    server_status_label_->setText("● 已连接");
    server_status_label_->setStyleSheet(
        "color: #50C878; font-size: 12px; padding: 4px 8px; "
        "background: rgba(80,200,120,0.15); border-radius: 10px;");
    statusBar()->showMessage("OK 已连接到信令服务器", 2000);

    // AuthById 现在在 LOGIN_RESP 成功后发送
}

void MainWindow::on_network_disconnected() {
    server_status_label_->setText("● 已断开");
    server_status_label_->setStyleSheet(
        "color: #E74C3C; font-size: 12px; padding: 4px 8px; "
        "background: rgba(231,76,60,0.15); border-radius: 10px;");
    statusBar()->showMessage("X 与服务器连接断开", 3000);
}

void MainWindow::on_network_error(const QString& err) {
    statusBar()->showMessage("网络错误: " + err, 3000);
}

// ── 网络消息处理 ───────────────────────────────────────────

void MainWindow::on_network_message(const std::string& data) {
    qDebug("MainWindow: on_network_message called, data size=%zu", data.size());
    process_incoming_message(data);
}

void MainWindow::process_incoming_message(const std::string& data) {
    // 解析 BaseMessage
    wemeet::BaseMessage base;
    if (!base.ParseFromString(data)) {
        qWarning("MainWindow: failed to parse BaseMessage");
        return;
    }

    qDebug("MainWindow: received message type=%d payload=%zu B, waiting_login_=%d",
           static_cast<int>(base.type()), base.payload().size(), waiting_login_);

    switch (base.type()) {
    case wemeet::MSG_AUTH_BY_ID_RESP: {
        wemeet::AuthByIdResp resp;
        if (resp.ParseFromString(base.payload()) && resp.success()) {
            qDebug("AuthById succeeded for user=%llu", resp.user_id());
        }
        break;
    }
    case wemeet::MSG_LOGIN_RESP: {
        wemeet::LoginResp resp;
        if (resp.ParseFromString(base.payload()) && waiting_login_) {
            waiting_login_ = false;
            if (resp.success()) {
                login_success_ = true;
                qDebug("LOGIN_RESP success: uid=%llu nick=%s",
                       resp.user_id(), resp.nickname().c_str());
                emit auth_success(resp.user_id(),
                                  QString::fromStdString(resp.nickname()));
                // 向服务器注册 user->conn 映射（AuthById）
                wemeet::AuthByIdReq areq;
                areq.set_user_id(resp.user_id());
                areq.set_nickname(resp.nickname());
                network_->send_message(encode_wrapped(
                    static_cast<int>(wemeet::MSG_AUTH_BY_ID_REQ),
                    0, areq));
            } else {
                QString err = QString::fromStdString(resp.error_msg().empty()
                    ? "邮箱或密码错误" : resp.error_msg());
                qDebug("LOGIN_RESP failed: %s", qPrintable(err));
                emit auth_failed(err);
            }
        } else if (!waiting_login_) {
            qWarning("MainWindow: received LOGIN_RESP but not waiting (dropped)");
        } else {
            qWarning("MainWindow: failed to parse LOGIN_RESP payload");
        }
        break;
    }
    case wemeet::MSG_REGISTER_RESP: {
        wemeet::RegisterResp resp;
        if (resp.ParseFromString(base.payload()) && waiting_register_) {
            waiting_register_ = false;
            QString err = QString::fromStdString(resp.error_msg());
            if (resp.success()) {
                qDebug("REGISTER_RESP success: uid=%llu", resp.user_id());
                emit register_result(true, "注册成功！用户ID: " +
                                     QString::number(resp.user_id()));
                // 注册成功后自动登录
                QString email = QString::fromStdString(err.isEmpty() ? "" : "");
                // err 在成功时为空，所以直接从最近一次的注册请求中获取
                // 这里简单实现：通知用户去登录
            } else {
                qDebug("REGISTER_RESP failed: %s", qPrintable(err));
                if (err.isEmpty()) err = "注册失败";
                emit register_result(false, err);
            }
        } else if (!waiting_register_) {
            qWarning("MainWindow: received REGISTER_RESP but not waiting (dropped)");
        } else {
            qWarning("MainWindow: failed to parse REGISTER_RESP payload");
        }
        break;
    }
    case wemeet::MSG_MEETING_CREATE_RESP: {
        wemeet::CreateMeetingResp resp;
        if (resp.ParseFromString(base.payload()) && resp.success()) {
            QString server_room_id = QString::fromStdString(resp.room_id());
            statusBar()->showMessage("会议已创建: " + server_room_id, 3000);

            // 弹窗显示服务器分配的房间号
            QMessageBox msgBox(this);
            msgBox.setWindowTitle("[+] 会议已创建");
            msgBox.setIcon(QMessageBox::Information);
            msgBox.setText("服务器已为分配会议号");
            msgBox.setInformativeText(
                QString("会议号：<b style='font-size:24px;color:#4A90D9;'>%1</b><br><br>"
                        "将此会议号发送给其他人，对方点击「加入会议」输入即可进入。")
                    .arg(server_room_id));
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();

            // 进入会议室（使用服务器房间号）
            if (pending_create_) {
                pending_create_ = false;
                enter_meeting(server_room_id);
                // 服务器已在创建会议时将创建者加入房间（如果逻辑支持）
                // 否则主动发送 JOIN_REQ
                QTimer::singleShot(200, this, [this, server_room_id]() {
                    send_join_meeting(server_room_id);
                });
            }
        } else if (resp.error_msg().size() > 0) {
            statusBar()->showMessage("创建会议失败: " +
                QString::fromStdString(resp.error_msg()), 5000);
            pending_create_ = false;
        }
        break;
    }
    case wemeet::MSG_MEETING_JOIN_RESP: {
        wemeet::JoinMeetingResp resp;
        if (resp.ParseFromString(base.payload())) {
            if (resp.success()) {
                statusBar()->showMessage(
                    QString("已加入会议 (当前 %1 人)").arg(resp.participants_size()), 3000);

                // JoinMeetingResp 不含 room_id，使用 pending_join_room_id_
                QString room_id = pending_join_room_id_;
                if (room_id.isEmpty()) room_id = current_room_id_;

                if (pending_join_) {
                    pending_join_ = false;
                    enter_meeting(room_id);
                }

                // 添加所有参与者（不重复添加本地用户）
                for (const auto& p : resp.participants()) {
                    bool is_self = (p.user_id() == current_user_id_);
                    meeting_page_->add_participant(
                        p.user_id(), QString::fromStdString(p.nickname()),
                        is_self, p.audio_on(), p.video_on(), p.is_host());

                    // 系统提示
                    if (!is_self) {
                        meeting_page_->append_chat_message(
                            0, "[系统]",
                            QString("* %1 已在会议中").arg(QString::fromStdString(p.nickname())));
                    }
                }

                // 自己加入成功的系统提示
                meeting_page_->append_chat_message(
                    0, "[系统]",
                    QString("OK 您已加入会议 (共 %1 人)").arg(resp.participants_size()));
            } else {
                statusBar()->showMessage("加入失败: " +
                    QString::fromStdString(resp.error_msg()), 5000);
                pending_join_ = false;
                QMessageBox::warning(this, "加入会议失败",
                    QString("无法加入会议: %1\n请检查会议号是否正确。")
                        .arg(QString::fromStdString(resp.error_msg())));
            }
        }
        break;
    }
    case wemeet::MSG_MEETING_PARTICIPANT: {
        wemeet::ParticipantUpdate update;
        if (update.ParseFromString(base.payload())) {
            const auto& p = update.participant();
            uint64_t uid = p.user_id();
            QString nick = QString::fromStdString(p.nickname());
            switch (update.action()) {
            case wemeet::ParticipantUpdate::JOINED:
                meeting_page_->add_participant(uid, nick, uid == current_user_id_,
                                                p.audio_on(), p.video_on(), p.is_host());
                statusBar()->showMessage(QString("%1 加入了会议").arg(nick), 3000);
                // 系统提示写入聊天框
                if (uid != current_user_id_) {
                    meeting_page_->append_chat_message(
                        0, "[系统]",
                        QString("* %1 加入了会议").arg(nick));
                }
                break;
            case wemeet::ParticipantUpdate::LEFT:
                meeting_page_->remove_participant(uid);
                statusBar()->showMessage(QString("%1 离开了会议").arg(nick), 3000);
                meeting_page_->append_chat_message(
                    0, "[系统]",
                    QString("* %1 离开了会议").arg(nick));
                break;
            case wemeet::ParticipantUpdate::UPDATED:
                meeting_page_->update_participant(uid, p.audio_on(), p.video_on());
                break;
            }
        }
        break;
    }
    case wemeet::MSG_CHAT_RECEIVE: {
        wemeet::ChatReceive msg;
        if (msg.ParseFromString(base.payload())) {
            meeting_page_->append_chat_message(
                msg.user_id(), QString::fromStdString(msg.nickname()),
                QString::fromStdString(msg.content()));
        }
        break;
    }
    case wemeet::MSG_MEDIA_RELAY_REGISTER_RESP: {
        // 收到中继注册响应，保存自己的 SSRC 与其他参与者的 SSRC
        wemeet::MediaRelayRegisterResp resp;
        if (resp.ParseFromString(base.payload())) {
            qDebug("MainWindow: MediaRelay registered, ssrc=%u, peers=%d",
                   resp.ssrc(), resp.peers_size());
            if (media_engine_) {
                // 设置本端 RTP 同步源（video/audio 共用同一个 ssrc）
                media_engine_->set_ssrc(resp.ssrc(), resp.ssrc());
                for (const auto& peer : resp.peers()) {
                    if (peer.user_id() != current_user_id_) {
                        media_engine_->set_peer_ssrc(peer.user_id(), peer.ssrc());
                    }
                }
            }
        }
        break;
    }
    case wemeet::MSG_BANDWIDTH_HINT: {
        // 服务端基于丢包率/RTT 下发的带宽调整建议
        wemeet::BandwidthHint hint;
        if (hint.ParseFromString(base.payload())) {
            qDebug("MainWindow: bandwidth hint -> %u kbps (min=%u, reason=%s)",
                   hint.max_bitrate_kbps(), hint.min_bitrate_kbps(),
                   hint.reason().c_str());
            if (media_engine_ && hint.max_bitrate_kbps() > 0) {
                // 应用服务端建议的上行码率上限
                media_engine_->apply_bandwidth_hint(hint.max_bitrate_kbps());
            }
        }
        break;
    }
    case wemeet::MSG_MEDIA_CONTROL: {
        // #20：远端屏幕共享暂停/恢复广播（服务端 MediaControl SCREEN 房间广播）
        // 约定：mute=true → 暂停共享，mute=false → 恢复共享
        wemeet::MediaControl ctrl;
        if (ctrl.ParseFromString(base.payload()) &&
            ctrl.media_type() == wemeet::MediaControl::SCREEN && in_meeting_) {
            bool paused = ctrl.mute();
            // self 守卫：共享者自身也会收到服务端直接回传的 fwd
            // （signaling_server.cpp:743 send_to_user(target_user_id)），
            // 跳过自身，避免在自己的 tile 上显示「对方已暂停共享」提示。
            if (ctrl.target_user_id() != current_user_id_) {
                // 路由到当前会议页：在共享者对应 tile 上叠加/解除「对方已暂停共享」
                meeting_page_->on_remote_share_paused(ctrl.target_user_id(), paused);
                qDebug("MainWindow: remote screen share %s user=%llu",
                       paused ? "PAUSED" : "RESUMED", ctrl.target_user_id());
            }
        }
        break;
    }
    default:
        break;
    }
}

// ── 发送消息 ───────────────────────────────────────────────

void MainWindow::send_create_meeting() {
    wemeet::CreateMeetingReq req;
    req.set_creator_id(current_user_id_);
    req.set_title(current_nickname_.toStdString() + "的会议");
    req.set_max_participants(50);
    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_MEETING_CREATE_REQ),
        QDateTime::currentMSecsSinceEpoch(), req));
}

void MainWindow::send_join_meeting(const QString& room_id) {
    wemeet::JoinMeetingReq req;
    req.set_user_id(current_user_id_);
    req.set_room_id(room_id.toStdString());
    req.set_nickname(current_nickname_.toStdString());
    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_MEETING_JOIN_REQ),
        QDateTime::currentMSecsSinceEpoch(), req));
}

void MainWindow::send_chat(const QString& content) {
    if (!in_meeting_ || current_room_id_.isEmpty()) return;
    wemeet::ChatSend req;
    req.set_user_id(current_user_id_);
    req.set_room_id(current_room_id_.toStdString());
    req.set_content(content.toStdString());
    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_CHAT_SEND),
        QDateTime::currentMSecsSinceEpoch(), req));
}

void MainWindow::send_leave_meeting() {
    if (current_room_id_.isEmpty()) return;
    wemeet::LeaveMeeting req;
    req.set_user_id(current_user_id_);
    req.set_room_id(current_room_id_.toStdString());
    network_->send_message(encode_wrapped(
        static_cast<int>(wemeet::MSG_MEETING_LEAVE),
        QDateTime::currentMSecsSinceEpoch(), req));
}

void MainWindow::send_logout() {
    if (network_->is_connected()) {
        wemeet::LogoutReq req;
        req.set_user_id(current_user_id_);
        network_->send_message(encode_wrapped(
            static_cast<int>(wemeet::MSG_LOGOUT_REQ),
            QDateTime::currentMSecsSinceEpoch(), req));
    }
}

// ── 主事件槽 ───────────────────────────────────────────────

void MainWindow::on_login_success(uint64_t user_id, const QString& nickname) {
    current_user_id_ = user_id;
    current_nickname_ = nickname;
    user_label_->setText(nickname);
    nav_bar_->show();
    nav_history_.clear();
    nav_history_.clear();
    stack_->setCurrentIndex(PAGE_LOBBY);
    update_back_button();

    // 连接到信令服务器
    connect_to_server();
}

void MainWindow::on_create_meeting() {
    if (!network_->is_connected()) {
        QMessageBox::warning(this, "未连接服务器",
            "请先连接到信令服务器后再创建会议。\n请确认服务正在运行。");
        return;
    }

    // 标记为等待服务器返回 room_id
    pending_create_ = true;
    statusBar()->showMessage("正在向服务器请求创建会议...");

    // 发送 CREATE_REQ，服务器返回的房间号通过 MSG_MEETING_CREATE_RESP 接收
    send_create_meeting();
}

void MainWindow::on_join_meeting(const QString& room_id) {
    if (!network_->is_connected()) {
        QMessageBox::warning(this, "未连接服务器",
            "请先连接到信令服务器后再加入会议。");
        return;
    }

    // 标记为等待服务器返回 JOIN_RESP
    pending_join_ = true;
    pending_join_room_id_ = room_id;
    statusBar()->showMessage(QString("正在加入会议 %1...").arg(room_id));

    // 发送 JOIN_REQ，服务器返回时再 set_room_info + navigate
    send_join_meeting(room_id);
}

void MainWindow::enter_meeting(const QString& room_id) {
    // 统一入口：在服务器返回房间号后调用
    current_room_id_ = room_id;
    in_meeting_ = true;

    // 初始化媒体引擎
    init_media_engine(current_user_id_, room_id, server_host_, 10000);

    // 设置会议室页面
    meeting_page_->set_media_engine(media_engine_);
    meeting_page_->set_room_info(room_id, "会议室: " + room_id,
                                  current_user_id_, current_nickname_);

    navigate_to(PAGE_MEETING);
}

void MainWindow::on_logout() {
    if (in_meeting_) {
        send_leave_meeting();
        in_meeting_ = false;
    }
    send_logout();

    if (media_engine_) media_engine_->shutdown();
    // 不调用 network_->disconnect()，保持连接，以便继续创建/加入会议
    // if (network_) network_->disconnect();

    current_room_id_.clear();
    nav_history_.clear();
    stack_->setCurrentIndex(PAGE_LOBBY);
    update_back_button();

    // 停留在大厅，保持连接状态
    qDebug("MainWindow: left meeting, returning to lobby");
}

void MainWindow::on_network_quality_changed(int32_t quality) {
    QString text;
    switch (quality) {
    case 5: text = "网络: 极佳"; break;
    case 4: text = "网络: 良好"; break;
    case 3: text = "网络: 一般"; break;
    case 2: text = "网络: 较差"; break;
    case 1: text = "网络: 极差"; break;
    default: text = "网络: 未知"; break;
    }
    statusBar()->showMessage("NET " + text, 2000);
}

void MainWindow::init_media_engine(uint64_t user_id, const QString& room_id,
                                    const QString& relay_host, uint16_t relay_port) {
    if (!media_engine_->initialize(user_id, room_id)) {
        qWarning("MainWindow: failed to initialize media engine");
        return;
    }
    media_engine_->register_media_relay(relay_host, relay_port);
    media_engine_->start_microphone();

    // ★ 先向服务器注册媒体中继（让服务器分配 ssrc），再启动摄像头
    // 否则 start_camera 发出的首批帧 ssrc=0 会被中继以"unknown ssrc"丢弃
    // ── 视频注册 ──
    {
        wemeet::MediaRelayRegister vreq;
        vreq.set_user_id(user_id);
        vreq.set_room_id(room_id.toStdString());
        vreq.set_relay_host(server_host_.toStdString());
        vreq.set_relay_port(media_engine_->local_video_port());
        vreq.set_media_type("video");
        network_->send_message(encode_wrapped(
            static_cast<int>(wemeet::MSG_MEDIA_RELAY_REGISTER),
            QDateTime::currentMSecsSinceEpoch(), vreq));
    }
    // ── 音频注册 ──
    {
        wemeet::MediaRelayRegister areq;
        areq.set_user_id(user_id);
        areq.set_room_id(room_id.toStdString());
        areq.set_relay_host(server_host_.toStdString());
        areq.set_relay_port(media_engine_->local_audio_port());
        areq.set_media_type("audio");
        network_->send_message(encode_wrapped(
            static_cast<int>(wemeet::MSG_MEDIA_RELAY_REGISTER),
            QDateTime::currentMSecsSinceEpoch(), areq));
    }

    // ★ 注册请求发出后再启动摄像头（TCP 顺序保证响应会在少量 ms 内到达）
    media_engine_->start_camera();
}
