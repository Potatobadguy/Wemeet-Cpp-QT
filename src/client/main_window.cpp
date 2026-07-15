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

// 辅助：序列化 Protobuf 子消息并包装为带 4 字节长度头的完整消息
std::string encode_wrapped(int msg_type, uint64_t seq_id, const google::protobuf::Message& payload) {
    std::string payload_bytes;
    payload.SerializeToString(&payload_bytes);

    wemeet::BaseMessage base;
    base.set_type(static_cast<wemeet::MsgType>(msg_type));
    base.set_sequence_id(seq_id);
    base.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    base.set_payload(payload_bytes);

    std::string base_bytes;
    base.SerializeToString(&base_bytes);

    // 4 字节大端长度头 + body
    uint32_t len = static_cast<uint32_t>(base_bytes.size());
    std::string out;
    out.resize(4 + len);
    out[0] = static_cast<char>((len >> 24) & 0xFF);
    out[1] = static_cast<char>((len >> 16) & 0xFF);
    out[2] = static_cast<char>((len >> 8) & 0xFF);
    out[3] = static_cast<char>(len & 0xFF);
    std::memcpy(&out[4], base_bytes.data(), len);
    return out;
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
        "QPushButton { color: white; padding: 14px 36px; "
        "border-radius: 10px; font-size: 15px; font-weight: bold; min-width: 200px; }";

    create_meeting_btn_ = new QPushButton("📅 创建会议");
    create_meeting_btn_->setStyleSheet(
        btn_style + "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #4A90D9, stop:1 #357ABD); }");

    auto* join_meeting_btn = new QPushButton("🔗 加入会议");
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

    back_btn_ = new QPushButton("←  返回");
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
    nav_layout->addWidget(user_label_);
    nav_layout->addStretch();
    nav_layout->addWidget(server_status_label_);
    nav_layout->addWidget(logout_btn_);

    nav_bar_->setStyleSheet("background: #ffffff; border-bottom: 1px solid #e8edf5;");
    nav_bar_->setFixedHeight(48);
    nav_bar_->hide();

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
    // 离开会议时通知服务器
    if (in_meeting_) {
        send_leave_meeting();
        in_meeting_ = false;
    }
    go_back();
}

// ── 服务器连接 ─────────────────────────────────────────────

void MainWindow::connect_to_server() {
    if (network_->is_connected()) return;
    statusBar()->showMessage(QString("正在连接 %1:%2...").arg(server_host_).arg(server_port_), 2000);
    network_->connect_to_server(server_host_, server_port_);
}

void MainWindow::on_network_connected() {
    server_status_label_->setText("● 已连接");
    server_status_label_->setStyleSheet(
        "color: #50C878; font-size: 12px; padding: 4px 8px; "
        "background: rgba(80,200,120,0.15); border-radius: 10px;");
    statusBar()->showMessage("✓ 已连接到信令服务器", 2000);

    // 免密码认证：通知服务器本用户的 user_id → conn_id 映射
    if (current_user_id_ > 0) {
        wemeet::AuthByIdReq req;
        req.set_user_id(current_user_id_);
        req.set_nickname(current_nickname_.toStdString());
        network_->send_message(encode_wrapped(
            static_cast<int>(wemeet::MSG_AUTH_BY_ID_REQ),
            QDateTime::currentMSecsSinceEpoch(), req));
        qDebug("AuthById sent: user=%llu", current_user_id_);
    }
}

void MainWindow::on_network_disconnected() {
    server_status_label_->setText("● 已断开");
    server_status_label_->setStyleSheet(
        "color: #E74C3C; font-size: 12px; padding: 4px 8px; "
        "background: rgba(231,76,60,0.15); border-radius: 10px;");
    statusBar()->showMessage("✗ 与服务器连接断开", 3000);
}

void MainWindow::on_network_error(const QString& err) {
    statusBar()->showMessage("网络错误: " + err, 3000);
}

// ── 网络消息处理 ───────────────────────────────────────────

void MainWindow::on_network_message(const std::string& data) {
    process_incoming_message(data);
}

void MainWindow::process_incoming_message(const std::string& data) {
    // 解析 BaseMessage
    wemeet::BaseMessage base;
    if (!base.ParseFromString(data)) {
        qWarning("MainWindow: failed to parse BaseMessage");
        return;
    }

    switch (base.type()) {
    case wemeet::MSG_AUTH_BY_ID_RESP: {
        wemeet::AuthByIdResp resp;
        if (resp.ParseFromString(base.payload()) && resp.success()) {
            qDebug("AuthById succeeded for user=%llu", resp.user_id());
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
            msgBox.setWindowTitle("📅 会议已创建");
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
                            QString("🟢 %1 已在会议中").arg(QString::fromStdString(p.nickname())));
                    }
                }

                // 自己加入成功的系统提示
                meeting_page_->append_chat_message(
                    0, "[系统]",
                    QString("✅ 您已加入会议 (共 %1 人)").arg(resp.participants_size()));
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
                        QString("🟢 %1 加入了会议").arg(nick));
                }
                break;
            case wemeet::ParticipantUpdate::LEFT:
                meeting_page_->remove_participant(uid);
                statusBar()->showMessage(QString("%1 离开了会议").arg(nick), 3000);
                meeting_page_->append_chat_message(
                    0, "[系统]",
                    QString("🔴 %1 离开了会议").arg(nick));
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
    if (network_) network_->disconnect();

    current_user_id_ = 0;
    current_nickname_.clear();
    current_room_id_.clear();
    nav_bar_->hide();
    nav_history_.clear();
    stack_->setCurrentIndex(PAGE_LOBBY);
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
    statusBar()->showMessage("📶 " + text, 2000);
}

void MainWindow::init_media_engine(uint64_t user_id, const QString& room_id,
                                    const QString& relay_host, uint16_t relay_port) {
    if (!media_engine_->initialize(user_id, room_id)) {
        qWarning("MainWindow: failed to initialize media engine");
        return;
    }
    media_engine_->register_media_relay(relay_host, relay_port);
    media_engine_->start_microphone();   // 麦克风通常可共享
    // 摄像头由 MediaEngine 内部自动尝试（独占时会失败并显示占位）
    media_engine_->start_camera();
}
