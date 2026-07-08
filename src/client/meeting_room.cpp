#include "meeting_room.h"
#include <QSplitter>
#include <QScrollArea>
#include <QLineEdit>

MeetingRoom::MeetingRoom(QWidget* parent)
    : QWidget(parent) {

    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);

    // 左侧: 视频画廊 + 控制栏
    auto* left_panel = new QWidget();
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(8, 8, 8, 8);

    // 房间标题
    room_title_ = new QLabel("会议室");
    room_title_->setStyleSheet("color: #eee; font-size: 18px; font-weight: bold; padding: 8px;");
    left_layout->addWidget(room_title_);

    // 视频画廊
    setup_video_gallery();
    left_layout->addLayout(gallery_layout_, 1);

    // 控制栏
    setup_control_bar();
    left_layout->addLayout(new QHBoxLayout()); // spacer

    // 右侧: 聊天面板
    setup_chat_panel();

    // 分屏
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(left_panel);
    splitter->addWidget(chat_display_->parentWidget());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    main_layout->addWidget(splitter);

    setStyleSheet("QWidget { background: #0a0a1a; }");
}

void MeetingRoom::set_room_info(const QString& room_id, const QString& title) {
    room_title_->setText(title + "  [" + room_id + "]");

    // 添加模拟参与者（演示用）
    add_participant(1, "我");
    add_participant(2, "张三");
    add_participant(3, "李四");
}

void MeetingRoom::add_participant(uint64_t user_id, const QString& nickname) {
    VideoTile tile;
    tile.user_id = user_id;

    tile.frame = new QFrame();
    tile.frame->setStyleSheet(
        "QFrame { background: #16213e; border: 2px solid #333; border-radius: 12px; }"
        "QFrame:hover { border-color: #4A90D9; }");
    tile.frame->setMinimumSize(240, 180);

    auto* inner = new QVBoxLayout(tile.frame);
    tile.label = new QLabel(nickname.mid(0, 1).toUpper());
    tile.label->setAlignment(Qt::AlignCenter);
    tile.label->setStyleSheet("color: #4A90D9; font-size: 36px; font-weight: bold;");

    tile.name_label = new QLabel(nickname);
    tile.name_label->setAlignment(Qt::AlignCenter);
    tile.name_label->setStyleSheet("color: #aaa; font-size: 12px; padding: 4px;");

    inner->addStretch();
    inner->addWidget(tile.label, 0, Qt::AlignCenter);
    inner->addStretch();
    inner->addWidget(tile.name_label);

    participants_.push_back(tile);
    update_gallery_layout();
}

void MeetingRoom::remove_participant(uint64_t user_id) {
    participants_.erase(
        std::remove_if(participants_.begin(), participants_.end(),
                        [user_id](const VideoTile& t) { return t.user_id == user_id; }),
        participants_.end());
    update_gallery_layout();
}

void MeetingRoom::update_gallery_layout() {
    // 清空旧布局
    while (gallery_layout_->count() > 0) {
        auto* item = gallery_layout_->takeAt(0);
        if (item->widget()) {
            item->widget()->hide();
            gallery_layout_->removeWidget(item->widget());
        }
        delete item;
    }

    // 自适应网格: 1/2/4/9等
    int count = static_cast<int>(participants_.size());
    int cols = 1;
    if (count > 1) cols = 2;
    if (count > 4) cols = 3;

    for (int i = 0; i < count; ++i) {
        int row = i / cols;
        int col = i % cols;
        gallery_layout_->addWidget(participants_[i].frame, row, col);
        participants_[i].frame->show();
    }
}

void MeetingRoom::setup_video_gallery() {
    gallery_layout_ = new QGridLayout();
    gallery_layout_->setSpacing(12);
}

void MeetingRoom::setup_control_bar() {
    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    bar_layout->setAlignment(Qt::AlignCenter);

    QString btn_base =
        "QPushButton { padding: 10px 20px; border-radius: 8px; font-size: 13px; "
        "color: white; border: none; min-width: 80px; }";

    mute_btn_ = new QPushButton("🎤 静音");
    mute_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }");
    bar_layout->addWidget(mute_btn_);

    video_btn_ = new QPushButton("📹 摄像头");
    video_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }");
    bar_layout->addWidget(video_btn_);

    share_btn_ = new QPushButton("🖥 共享");
    share_btn_->setStyleSheet(btn_base + "QPushButton { background: #444; }");
    bar_layout->addWidget(share_btn_);

    hangup_btn_ = new QPushButton("📞 挂断");
    hangup_btn_->setStyleSheet(btn_base + "QPushButton { background: #E74C3C; }"
                               "QPushButton:hover { background: #C0392B; }");

    connect(mute_btn_,  &QPushButton::clicked, this, &MeetingRoom::on_mute_toggled);
    connect(video_btn_, &QPushButton::clicked, this, &MeetingRoom::on_video_toggled);
    connect(hangup_btn_, &QPushButton::clicked, this, &MeetingRoom::on_hangup);

    bar_layout->addWidget(hangup_btn_);
    bar_layout->addStretch();
}

void MeetingRoom::setup_chat_panel() {
    auto* panel = new QWidget();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);

    chat_display_ = new QTextEdit();
    chat_display_->setReadOnly(true);
    chat_display_->setStyleSheet(
        "QTextEdit { background: #111122; color: #ccc; border: none; "
        "border-radius: 4px; padding: 8px; font-size: 12px; }");
    layout->addWidget(chat_display_);

    auto* input_row = new QHBoxLayout();
    chat_input_ = new QLineEdit();
    chat_input_->setPlaceholderText("输入消息...");
    chat_input_->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #eee; border: 1px solid #333; "
        "border-radius: 4px; padding: 8px; }");
    input_row->addWidget(chat_input_);

    send_btn_ = new QPushButton("发送");
    send_btn_->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 8px 16px; "
        "border-radius: 4px; }"
        "QPushButton:hover { background: #357ABD; }");
    input_row->addWidget(send_btn_);

    layout->addLayout(input_row);

    connect(send_btn_, &QPushButton::clicked, this, &MeetingRoom::on_send_chat);
    connect(chat_input_, &QLineEdit::returnPressed, this, &MeetingRoom::on_send_chat);
}

// ── 槽函数 ───────────────────────────────────────────────
void MeetingRoom::on_mute_toggled() {
    mic_muted_ = !mic_muted_;
    mute_btn_->setText(mic_muted_ ? "🔇 已静音" : "🎤 静音");
    mute_btn_->setStyleSheet(mic_muted_
        ? "QPushButton { background: #E74C3C; color: white; padding: 10px 20px; border-radius: 8px; }"
        : "QPushButton { background: #444; color: white; padding: 10px 20px; border-radius: 8px; }");
}

void MeetingRoom::on_video_toggled() {
    video_off_ = !video_off_;
    video_btn_->setText(video_off_ ? "📷 已关闭" : "📹 摄像头");
    video_btn_->setStyleSheet(video_off_
        ? "QPushButton { background: #E74C3C; color: white; padding: 10px 20px; border-radius: 8px; }"
        : "QPushButton { background: #444; color: white; padding: 10px 20px; border-radius: 8px; }");
}

void MeetingRoom::on_hangup() {
    emit leave_meeting();
}

void MeetingRoom::on_send_chat() {
    QString msg = chat_input_->text().trimmed();
    if (msg.isEmpty()) return;

    chat_display_->append("<b style='color:#4A90D9'>我:</b> " + msg);
    chat_input_->clear();
}
