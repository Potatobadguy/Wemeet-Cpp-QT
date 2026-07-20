#include "meeting_room.h"
#include "media_engine.h"
#include "rtp_session.h"
#include <algorithm>
#include <QSplitter>
#include <QScrollArea>
#include <QLineEdit>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QToolTip>
#include <QResizeEvent>
#include <QListWidget>
#include <memory>

// ── 构造函数/析构函数 ───────────────────────────────────────

MeetingRoom::MeetingRoom(QWidget* parent)
    : QWidget(parent) {
    setup_ui();
}

MeetingRoom::~MeetingRoom() {
    if (media_engine_) {
        media_engine_->shutdown();
    }
}

void MeetingRoom::setup_ui() {
    main_layout_ = new QVBoxLayout(this);
    main_layout_->setContentsMargins(0, 0, 0, 0);
    main_layout_->setSpacing(0);

    // 内容区（画廊 + 聊天 + 成员面板）
    content_stack_ = new QStackedWidget(this);

    // ── 画廊页 ──────────────────────────────────────────
    auto* gallery_page = new QWidget();
    auto* gallery_page_layout = new QHBoxLayout(gallery_page);
    gallery_page_layout->setContentsMargins(8, 8, 8, 8);

    // 左侧: 房间标题 + 视频画廊 + 控制栏
    auto* left_panel = new QWidget();
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 8, 0);

    // 标题行: 标题 + 质量指示
    auto* title_row = new QHBoxLayout();
    room_title_ = new QLabel("会议室");
    room_title_->setStyleSheet("color: #eee; font-size: 18px; font-weight: bold; padding: 8px;");

    quality_indicator_ = new QLabel("● 良好");
    quality_indicator_->setStyleSheet("color: #50C878; font-size: 12px; padding: 4px 8px; "
                                       "background: rgba(80,200,120,0.15); border-radius: 10px;");
    quality_indicator_->setToolTip("网络质量");

    // 参会人数指示
    participant_count_label_ = new QLabel(" 0");
    participant_count_label_->setStyleSheet(
        "color: #aaa; font-size: 12px; padding: 4px 8px; "
        "background: rgba(255,255,255,0.1); border-radius: 10px;");
    title_row->insertWidget(1, participant_count_label_);
    title_row->addWidget(room_title_);
    title_row->addStretch();
    title_row->addWidget(quality_indicator_);
    left_layout->addLayout(title_row);

    // 画廊滚动区
    gallery_scroll_ = new QScrollArea();
    gallery_scroll_->setWidgetResizable(true);
    gallery_scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; } "
                                    "QScrollBar:vertical { width: 6px; background: #1a1a2e; } "
                                    "QScrollBar::handle:vertical { background: #4A90D9; border-radius: 3px; }");

    gallery_container_ = new QWidget();
    gallery_layout_ = new QGridLayout(gallery_container_);
    gallery_layout_->setSpacing(12);
    gallery_scroll_->setWidget(gallery_container_);
    left_layout->addWidget(gallery_scroll_, 1);

    // 控制栏
    auto* bar_layout = new QHBoxLayout();
    bar_layout->setAlignment(Qt::AlignCenter);
    bar_layout->setSpacing(12);

    QString btn_base =
        "QPushButton { padding: 10px 20px; border-radius: 8px; font-size: 13px; "
        "color: white; border: none; min-width: 80px; font-weight: bold; }";

    mute_btn_ = new QPushButton("MIC 静音");
    mute_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }"
                              "QPushButton:hover { background: #555; }");
    mute_btn_->setCursor(Qt::PointingHandCursor);

    video_btn_ = new QPushButton("CAM 摄像头");
    video_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }"
                               "QPushButton:hover { background: #555; }");
    video_btn_->setCursor(Qt::PointingHandCursor);

    share_btn_ = new QPushButton("SCR 共享");
    share_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }"
                               "QPushButton:hover { background: #555; }");
    share_btn_->setCursor(Qt::PointingHandCursor);

    members_btn_ = new QPushButton("USR 成员");
    members_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }"
                                 "QPushButton:hover { background: #555; }");
    members_btn_->setCursor(Qt::PointingHandCursor);

    back_btn_ = new QPushButton("⬅");
    back_btn_->setToolTip("返回大厅");
    back_btn_->setStyleSheet(btn_base + "QPushButton { background: #666; }"
                              "QPushButton:hover { background: #555; }");
    back_btn_->setCursor(Qt::PointingHandCursor);

    hangup_btn_ = new QPushButton("END 挂断");
    hangup_btn_->setStyleSheet(btn_base + "QPushButton { background: #E74C3C; }"
                                "QPushButton:hover { background: #C0392B; }");
    hangup_btn_->setCursor(Qt::PointingHandCursor);

    bar_layout->addWidget(mute_btn_);

    // 麦克风音量指示条（细条，左侧为控制栏按钮区域）
    volume_bar_ = new QProgressBar();
    volume_bar_->setRange(0, 100);
    volume_bar_->setValue(0);
    volume_bar_->setFixedWidth(80);
    volume_bar_->setFixedHeight(8);
    volume_bar_->setTextVisible(false);
    volume_bar_->setStyleSheet(
        "QProgressBar { background: #333; border-radius: 4px; border: none; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "stop:0 #50C878, stop:0.5 #FFD700, stop:1 #E74C3C); border-radius: 4px; }");
    volume_bar_->setToolTip("麦克风音量");
    bar_layout->addWidget(volume_bar_);

    bar_layout->addWidget(video_btn_);
    bar_layout->addWidget(share_btn_);
    bar_layout->addWidget(members_btn_);
    bar_layout->addWidget(back_btn_);
    bar_layout->addWidget(hangup_btn_);

    left_layout->addLayout(bar_layout);
    gallery_page_layout->addWidget(left_panel, 3);

    // ── 聊天面板 ────────────────────────────────────────
    chat_panel_ = new QWidget();
    auto* chat_layout = new QVBoxLayout(chat_panel_);
    chat_layout->setContentsMargins(0, 0, 0, 0);

    auto* chat_title = new QLabel("CHAT 聊天");
    chat_title->setStyleSheet("color: #eee; font-size: 14px; font-weight: bold; "
                               "padding: 8px; background: #1a1a2e; border-radius: 4px;");

    chat_display_ = new QTextEdit();
    chat_display_->setReadOnly(true);
    chat_display_->setPlaceholderText("等待消息...");
    chat_display_->setStyleSheet(
        "QTextEdit { background: #111122; color: #ccc; border: none; "
        "border-radius: 4px; padding: 8px; font-size: 12px; }");

    auto* input_row = new QHBoxLayout();
    input_row->setSpacing(6);
    chat_input_ = new QLineEdit();
    chat_input_->setPlaceholderText("输入消息...");
    chat_input_->setClearButtonEnabled(true);
    chat_input_->setMinimumHeight(40);
    chat_input_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    chat_input_->setFocusPolicy(Qt::StrongFocus);
    chat_input_->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #eee; border: 1px solid #444; "
        "border-radius: 6px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: #4A90D9; background: #232336; }");

    send_btn_ = new QPushButton("发送");
    send_btn_->setMinimumHeight(40);
    send_btn_->setMinimumWidth(60);
    send_btn_->setCursor(Qt::PointingHandCursor);
    send_btn_->setFocusPolicy(Qt::NoFocus);  // 不抢输入框的 Enter
    send_btn_->setAutoDefault(false);
    send_btn_->setDefault(false);
    send_btn_->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 8px 16px; "
        "border-radius: 6px; font-weight: bold; font-size: 13px; }"
        "QPushButton:hover { background: #357ABD; }"
        "QPushButton:pressed { background: #2A5F9E; }");

    input_row->addWidget(chat_input_, 1);
    input_row->addWidget(send_btn_);

    chat_layout->addWidget(chat_title);
    chat_layout->addWidget(chat_display_, 1);
    chat_layout->addLayout(input_row);

    // 让 input_row 不会被压扁
    chat_layout->setStretchFactor(chat_display_, 1);
    chat_layout->setStretchFactor(input_row, 0);

    gallery_page_layout->addWidget(chat_panel_, 1);

    content_stack_->addWidget(gallery_page);

    // ── 成员列表页 ──────────────────────────────────────
    member_panel_ = new QWidget();
    auto* member_layout = new QVBoxLayout(member_panel_);

    auto* member_title = new QLabel("USR 参会成员");
    member_title->setStyleSheet("color: #eee; font-size: 16px; font-weight: bold; "
                                 "padding: 12px; background: #1a1a2e;");

    member_list_ = new QListWidget();
    member_list_->setStyleSheet(
        "QListWidget { background: #111122; color: #ccc; border: none; "
        "border-radius: 4px; font-size: 13px; }"
        "QListWidget::item { padding: 12px; border-bottom: 1px solid #222; }"
        "QListWidget::item:hover { background: #1a1a2e; }");

    auto* member_back_btn = new QPushButton("<- 返回画廊");
    member_back_btn->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 10px; "
        "border-radius: 4px; font-weight: bold; }");

    member_layout->addWidget(member_title);
    member_layout->addWidget(member_list_, 1);
    member_layout->addWidget(member_back_btn);

    content_stack_->addWidget(member_panel_);

    main_layout_->addWidget(content_stack_, 1);

    // ── 信号连接 ────────────────────────────────────────
    connect(mute_btn_,    &QPushButton::clicked, this, &MeetingRoom::on_mute_toggled);
    connect(video_btn_,   &QPushButton::clicked, this, &MeetingRoom::on_video_toggled);
    connect(share_btn_,   &QPushButton::clicked, this, &MeetingRoom::on_share_toggled);
    connect(members_btn_, &QPushButton::clicked, this, [this]() {
        content_stack_->setCurrentIndex(1);
        rebuild_member_list();
    });
    connect(member_back_btn, &QPushButton::clicked, this, [this]() {
        content_stack_->setCurrentIndex(0);
    });
    connect(back_btn_,   &QPushButton::clicked, this, &MeetingRoom::on_back);
    connect(hangup_btn_, &QPushButton::clicked, this, &MeetingRoom::on_hangup);
    connect(send_btn_,   &QPushButton::clicked, this, &MeetingRoom::on_send_chat);
    connect(chat_input_, &QLineEdit::returnPressed, this, &MeetingRoom::on_send_chat);
    connect(member_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        uint64_t uid = item->data(Qt::UserRole).toULongLong();
        if (uid != local_user_id_ && is_host_) {
            on_participant_context_menu(uid);
        }
    });

    setStyleSheet("QWidget { background: #0a0a1a; }");
}

// ── 设置 ────────────────────────────────────────────────────

void MeetingRoom::set_room_info(const QString& room_id, const QString& title,
                                 uint64_t local_user_id, const QString& nickname) {
    room_id_ = room_id;
    room_title_str_ = title;
    local_user_id_ = local_user_id;
    nickname_ = nickname;

    room_title_->setText(title + "  [" + room_id + "]");

    // 进入会议室后自动聚焦到聊天输入框（方便用户立即发言）
    QTimer::singleShot(300, this, [this]() {
        if (chat_input_) chat_input_->setFocus();
    });

    // 清理后重新填充
    participants_.clear();
    add_participant(local_user_id, nickname, true, true, true, true);

    qDebug("MeetingRoom: room=%s, user=%llu, nick=%s",
           qPrintable(room_id), local_user_id, qPrintable(nickname));
}

void MeetingRoom::set_media_engine(MediaEngine* engine) {
    media_engine_ = engine;
    if (!media_engine_) return;

    // 连接媒体引擎信号到 UI
    connect(media_engine_, &MediaEngine::network_quality_changed,
            this, &MeetingRoom::update_network_quality);

    // 麦克风音量指示器
    connect(media_engine_, &MediaEngine::volume_level_changed,
            this, &MeetingRoom::on_volume_level_changed);

    connect(media_engine_, &MediaEngine::remote_video_frame,
            this, [this](uint64_t user_id, const QVideoFrame& frame) {
        auto* tile = find_tile(user_id);
        if (tile && tile->video_widget) {
            tile->video_widget->present_frame(frame);
        }
    });
}

// ── 参与者管理 ─────────────────────────────────────────────

void MeetingRoom::add_participant(uint64_t user_id, const QString& nickname,
                                   bool is_local, bool audio_on,
                                   bool video_on, bool is_host) {
    // 已存在不重复添加
    if (find_tile(user_id)) return;

    VideoTile tile;
    tile.user_id   = user_id;
    tile.nickname  = nickname;
    tile.is_local  = is_local;
    tile.video_on  = video_on;
    tile.audio_on  = audio_on;
    tile.is_host   = is_host;

    if (user_id == local_user_id_) {
        is_host_ = is_host;
    }

    tile.frame = new QFrame(gallery_container_);
    apply_tile_style(tile);

    auto* inner = new QVBoxLayout(tile.frame);
    inner->setContentsMargins(8, 8, 8, 8);
    inner->setSpacing(4);

    // 视频/头像区
    auto* video_container = new QWidget();
    auto* video_layout = new QStackedLayout(video_container);
    video_layout->setStackingMode(QStackedLayout::StackAll);

    // 头像占位
    tile.avatar_label = new QLabel(nickname.mid(0, 1).toUpper());
    tile.avatar_label->setAlignment(Qt::AlignCenter);
    tile.avatar_label->setStyleSheet("color: #4A90D9; font-size: 36px; font-weight: bold;"
                                      "background: transparent;");

    // 视频控件（本地用 QVideoWidget / 远端用 RemoteVideoWidget）
    if (is_local) {
        tile.video_widget = nullptr;
        inner->addWidget(video_container, 1);
        video_layout->addWidget(tile.avatar_label);

        // 摄像头可用时创建本地预览控件
        if (media_engine_ && media_engine_->camera_active()) {
            auto* local_video = new QVideoWidget();
            local_video->setStyleSheet("background: black; border-radius: 10px;");
            local_video->setMinimumSize(200, 150);
            video_layout->addWidget(local_video);
            media_engine_->set_local_preview(local_video);
            tile.avatar_label->setVisible(false);
        } else {
            // 摄像头不可用（独占 / 无设备）— 增强占位
            tile.avatar_label->setStyleSheet(
                "color: #E67E22; font-size: 32px; font-weight: bold;"
                "background: transparent;");
            tile.avatar_label->setText(nickname.mid(0, 1).toUpper() + "\n\n(无视频)");
            tile.avatar_label->setToolTip(
                media_engine_ && !media_engine_->camera_active()
                    ? "摄像头被其他应用占用或不可用"
                    : "未检测到摄像头");
        }
    } else {
        // 远端用户 — 使用 RemoteVideoWidget
        tile.video_widget = nullptr;
        if (media_engine_) {
            tile.video_widget = media_engine_->create_remote_video_widget(user_id, video_container);
            tile.video_widget->setMinimumSize(200, 150);
            tile.video_widget->set_video_on(video_on);
            tile.video_widget->set_placeholder_text(nickname);
            video_layout->addWidget(tile.avatar_label);
            video_layout->addWidget(tile.video_widget);
        } else {
            video_layout->addWidget(tile.avatar_label);
        }
        inner->addWidget(video_container, 1);
    }

    if (!video_on) {
        tile.avatar_label->show();
    }

    // 底部信息栏
    auto* info_row = new QHBoxLayout();

    // 静音指示器
    tile.mute_indicator = new QPushButton();
    tile.mute_indicator->setFixedSize(24, 24);
    tile.mute_indicator->setStyleSheet(
        audio_on ? "QPushButton { background: transparent; font-size: 14px; }"
                 : "QPushButton { background: #E74C3C; color: white; border-radius: 12px; font-size: 12px; }");
    tile.mute_indicator->setText(audio_on ? "SOUND" : "MUTE");
    tile.mute_indicator->setFlat(true);
    tile.mute_indicator->setEnabled(false);

    tile.name_label = new QLabel(nickname + (is_host ? " *" : ""));
    tile.name_label->setAlignment(Qt::AlignCenter);
    tile.name_label->setStyleSheet("color: #aaa; font-size: 12px; padding: 2px;"
                                    "background: transparent;");

    tile.quality_icon = new QLabel("●");
    tile.quality_icon->setStyleSheet("color: #50C878; font-size: 10px; background: transparent;");
    tile.quality_icon->setToolTip("网络质量良好");

    info_row->addStretch();
    info_row->addWidget(tile.mute_indicator);
    info_row->addWidget(tile.name_label);
    info_row->addWidget(tile.quality_icon);
    info_row->addStretch();

    inner->addLayout(info_row);

    participants_.push_back(tile);
    participant_count_label_->setText(QString(" %1").arg(participants_.size()));
    rebuild_gallery();
}

void MeetingRoom::remove_participant(uint64_t user_id) {
    auto it = std::remove_if(participants_.begin(), participants_.end(),
                              [user_id](const VideoTile& t) { return t.user_id == user_id; });
    if (it != participants_.end()) {
        // 清理远端视频控件
        if (media_engine_) {
            media_engine_->remove_remote_video_widget(user_id);
        }
        participants_.erase(it, participants_.end());
        participant_count_label_->setText(QString(" %1").arg(participants_.size()));
        rebuild_gallery();
    }
}

void MeetingRoom::update_participant(uint64_t user_id, bool audio_on, bool video_on) {
    auto* tile = find_tile(user_id);
    if (!tile) return;

    tile->audio_on = audio_on;
    tile->video_on = video_on;

    if (tile->mute_indicator) {
        tile->mute_indicator->setText(audio_on ? "SOUND" : "MUTE");
        tile->mute_indicator->setStyleSheet(
            audio_on ? "QPushButton { background: transparent; font-size: 14px; }"
                     : "QPushButton { background: #E74C3C; color: white; border-radius: 12px; font-size: 12px; }");
    }

    if (tile->video_widget) {
        tile->video_widget->set_video_on(video_on);
    }

    if (!video_on && tile->avatar_label) {
        tile->avatar_label->show();
    }

    update_member_list();
}

// ── 屏幕共享 ────────────────────────────────────────────────

void MeetingRoom::on_screen_share_started(uint64_t user_id, const QString& nickname) {
    qDebug("Screen share started by %llu (%s)", user_id, qPrintable(nickname));
    // 在画廊中高亮共享者
    for (auto& tile : participants_) {
        if (tile.user_id == user_id) {
            tile.frame->setStyleSheet(
                "QFrame { background: #16213e; border: 3px solid #4A90D9; "
                "border-radius: 12px; }");
        }
    }
}

void MeetingRoom::on_screen_share_stopped(uint64_t user_id) {
    auto* tile = find_tile(user_id);
    if (tile) {
        apply_tile_style(*tile);
    }
}

// ── 网络质量 ────────────────────────────────────────────────

void MeetingRoom::update_network_quality(int32_t quality) {
    network_quality_ = quality;
    QString text, color;

    switch (quality) {
    case 5: text = "● 极佳"; color = "#50C878"; break;
    case 4: text = "● 良好"; color = "#90EE90"; break;
    case 3: text = "● 一般"; color = "#FFD700"; break;
    case 2: text = "● 较差"; color = "#FF8C00"; break;
    case 1: text = "● 极差"; color = "#FF4500"; break;
    default: text = "● 未知"; color = "#888"; break;
    }

    quality_indicator_->setText(text);
    quality_indicator_->setStyleSheet(
        QString("color: %1; font-size: 12px; padding: 4px 8px; "
                "background: rgba(%2,%3,%4,0.15); border-radius: 10px;")
            .arg(color)
            .arg(quality >= 4 ? "80,200,120" : quality >= 3 ? "255,215,0" : "255,69,0"));

    // 更新每个 tile 的质量指示
    for (auto& tile : participants_) {
        if (tile.quality_icon) {
            tile.quality_icon->setStyleSheet(
                QString("color: %1; font-size: 10px; background: transparent;").arg(color));
        }
    }
}

// ── 槽函数 ──────────────────────────────────────────────────

void MeetingRoom::on_mute_toggled() {
    mic_muted_ = !mic_muted_;
    mute_btn_->setText(mic_muted_ ? "MUTE 已静音" : "MIC 静音");
    mute_btn_->setStyleSheet(mic_muted_
        ? "QPushButton { background: #E74C3C; color: white; padding: 10px 20px; "
          "border-radius: 8px; font-weight: bold; }"
        : "QPushButton { background: #444; color: white; padding: 10px 20px; "
          "border-radius: 8px; font-weight: bold; }");

    if (media_engine_) {
        media_engine_->mute_audio(mic_muted_);
    }
    update_participant(local_user_id_, !mic_muted_, !video_off_);
    emit mute_toggled(mic_muted_);
}

void MeetingRoom::on_video_toggled() {
    video_off_ = !video_off_;
    video_btn_->setText(video_off_ ? "CAM 已关闭" : "CAM 摄像头");
    video_btn_->setStyleSheet(video_off_
        ? "QPushButton { background: #E74C3C; color: white; padding: 10px 20px; "
          "border-radius: 8px; font-weight: bold; }"
        : "QPushButton { background: #444; color: white; padding: 10px 20px; "
          "border-radius: 8px; font-weight: bold; }");

    if (media_engine_) {
        media_engine_->mute_video(video_off_);
        if (video_off_) {
            media_engine_->stop_camera();
        } else {
            media_engine_->start_camera();
        }
    }

    auto* tile = find_tile(local_user_id_);
    if (tile) {
        tile->video_on = !video_off_;
        if (tile->avatar_label) {
            tile->avatar_label->setVisible(video_off_);
        }
    }

    emit video_toggled(video_off_);
}

void MeetingRoom::on_share_toggled() {
    if (!media_engine_) return;

    if (sharing_) {
        // 停止共享
        media_engine_->stop_screen_share();
        share_btn_->setText("SCR 共享");
        share_btn_->setStyleSheet(
            "QPushButton { background: #444; color: white; padding: 10px 20px; "
            "border-radius: 8px; font-weight: bold; }");
        sharing_ = false;
    } else {
        // 选择屏幕开始共享
        QList<QScreen*> screens = QGuiApplication::screens();
        if (screens.isEmpty()) return;

        int screen_idx = 0;
        if (screens.size() > 1) {
            QStringList items;
            for (int i = 0; i < screens.size(); ++i) {
                auto* s = screens[i];
                items << QString("屏幕 %1: %2x%3").arg(i).arg(s->size().width()).arg(s->size().height());
            }
            bool ok;
            QString item = QInputDialog::getItem(this, "选择共享屏幕", "屏幕:", items, 0, false, &ok);
            if (!ok) return;
            screen_idx = items.indexOf(item);
            if (screen_idx < 0) return;
        }

        media_engine_->start_screen_share(screen_idx);
        share_btn_->setText("⏹ 停止共享");
        share_btn_->setStyleSheet(
            "QPushButton { background: #E67E22; color: white; padding: 10px 20px; "
            "border-radius: 8px; font-weight: bold; }");
        sharing_ = true;
    }
    emit screen_share_toggled(sharing_);
}

void MeetingRoom::on_volume_level_changed(double level) {
    if (mic_muted_) {
        volume_bar_->setValue(0);
        return;
    }
    int val = static_cast<int>(level * 100);
    volume_bar_->setValue(std::min(100, val));
}

void MeetingRoom::on_hangup() {
    if (media_engine_) {
        media_engine_->shutdown();
    }
    emit leave_meeting();
}

void MeetingRoom::on_back() {
    if (media_engine_) {
        media_engine_->shutdown();
    }
    emit back_to_lobby();
}

void MeetingRoom::on_send_chat() {
    QString msg = chat_input_->text().trimmed();
    if (msg.isEmpty()) return;

    chat_input_->clear();

    // 本地立即显示（不依赖服务器回显）
    append_chat_message(local_user_id_, nickname_ + " (我)", msg);

    // 发出信号，让 MainWindow 通过网络发送给服务器
    emit send_chat_text(msg);
}

void MeetingRoom::append_chat_message(uint64_t user_id, const QString& sender_nick,
                                       const QString& content) {
    bool is_self = (user_id == local_user_id_);
    QString color = is_self ? "#4A90D9" : "#50C878";
    chat_display_->append(
        QString("<div style='color:%1; font-weight:bold;'>%2:</div>"
                "<div style='color:#ddd; padding-left:12px; padding-bottom:4px;'>%3</div>")
            .arg(color, sender_nick.isEmpty() ? QString::number(user_id) : sender_nick, content));
}

void MeetingRoom::on_participant_context_menu(uint64_t user_id) {
    if (!is_host_) return;

    auto* tile = find_tile(user_id);
    if (!tile) return;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background: #1a1a2e; color: #eee; border: 1px solid #333; "
        "border-radius: 6px; padding: 4px; }"
        "QMenu::item { padding: 8px 24px; border-radius: 4px; }"
        "QMenu::item:hover { background: #4A90D9; }");

    QAction* mute_audio_act = menu.addAction(
        tile->audio_on ? "MUTE 静音麦克风" : "SOUND 取消静音");
    QAction* mute_video_act = menu.addAction(
        tile->video_on ? "CAM 关闭摄像头" : "CAM 开启摄像头");
    menu.addSeparator();
    QAction* set_host_act = menu.addAction("* 设为主持人");
    QAction* remove_act = menu.addAction("X 移出会议");

    QAction* selected = menu.exec(QCursor::pos());
    if (selected == mute_audio_act) {
        emit role_changed(user_id, tile->audio_on ? 0 : 1);
    } else if (selected == mute_video_act) {
        emit role_changed(user_id, tile->video_on ? 0 : 1);
    } else if (selected == set_host_act) {
        emit role_changed(user_id, 2);
    } else if (selected == remove_act) {
        emit role_changed(user_id, 3);
    }
}

// ── 画廊布局 ────────────────────────────────────────────────

void MeetingRoom::rebuild_gallery() {
    // 清空
    while (gallery_layout_->count() > 0) {
        auto* item = gallery_layout_->takeAt(0);
        if (item->widget()) {
            item->widget()->hide();
            gallery_layout_->removeWidget(item->widget());
        }
        delete item;
    }

    int count = static_cast<int>(participants_.size());
    if (count == 0) return;

    int cols = gallery_columns();
    for (int i = 0; i < count; ++i) {
        int row = i / cols;
        int col = i % cols;
        gallery_layout_->addWidget(participants_[i].frame, row, col);
        participants_[i].frame->show();
    }

    update_member_list();
}

int MeetingRoom::gallery_columns() const {
    int count = static_cast<int>(participants_.size());
    if (is_mobile_layout()) return 1;
    if (count <= 1) return 1;
    if (count <= 4) return 2;
    if (count <= 9) return 3;
    return 4;
}

MeetingRoom::VideoTile* MeetingRoom::find_tile(uint64_t user_id) {
    for (auto& tile : participants_) {
        if (tile.user_id == user_id) return &tile;
    }
    return nullptr;
}

void MeetingRoom::apply_tile_style(VideoTile& tile) {
    tile.frame->setStyleSheet(
        "QFrame { background: #16213e; border: 2px solid #333; border-radius: 12px; }"
        "QFrame:hover { border-color: #4A90D9; }");
    tile.frame->setMinimumSize(200, 160);
}

void MeetingRoom::rebuild_member_list() {
    member_list_->clear();
    for (auto& p : participants_) {
        QString text = p.nickname;
        if (p.is_host) text += " *";
        text += p.audio_on ? "  SOUND" : "  MUTE";
        text += p.video_on ? "  CAM" : "  CAM";

        auto* item = new QListWidgetItem(text, member_list_);
        item->setData(Qt::UserRole, static_cast<qulonglong>(p.user_id));
    }
}

void MeetingRoom::update_member_list() {
    // 如果成员面板可见则刷新
    if (content_stack_->currentIndex() == 1) {
        rebuild_member_list();
    }
}

// ── 自适应布局 ──────────────────────────────────────────────

void MeetingRoom::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // 移动端布局适配
    bool mobile = is_mobile_layout();
    chat_panel_->setVisible(!mobile);
    members_btn_->setVisible(mobile);

    // 宽度窄时堆叠控制栏
    if (width() < 500) {
        for (auto* btn : {mute_btn_, video_btn_, share_btn_, members_btn_, back_btn_, hangup_btn_}) {
            btn->setMinimumWidth(50);
            btn->setMaximumWidth(60);
        }
    } else {
        for (auto* btn : {mute_btn_, video_btn_, share_btn_, members_btn_, back_btn_, hangup_btn_}) {
            btn->setMinimumWidth(80);
            btn->setMaximumWidth(16777215);
        }
    }

    rebuild_gallery();
}
