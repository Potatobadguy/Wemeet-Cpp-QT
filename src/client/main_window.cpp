#include "main_window.h"
#include "meeting_room.h"
#include "network_client.h"
#include <QMessageBox>
#include <QInputDialog>


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , network_(new NetworkClient(this)) {

    setup_navigation();   // 先创建 nav_bar_，setup_ui 中会用到
    setup_ui();

    // 默认显示大厅页
    stack_->setCurrentIndex(PAGE_LOBBY);
}

MainWindow::~MainWindow() = default;

void MainWindow::setup_ui() {
    // ── 中央容器：导航栏(上) + 页面堆叠器(下) ────────────────
    QWidget* central = new QWidget(this);
    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    root_layout->addWidget(nav_bar_, 0);      // 导航栏：固定高度 48px
    stack_ = new QStackedWidget(this);
    root_layout->addWidget(stack_, 1);         // 页面区：占满剩余空间

    setCentralWidget(central);

    // ── 大厅页 ───────────────────────────────────────────
    lobby_page_ = new QWidget(this);
    auto* lobby_layout = new QVBoxLayout(lobby_page_);

    create_meeting_btn_ = new QPushButton("创建会议");
    create_meeting_btn_->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 12px 24px; "
        "border-radius: 8px; font-size: 14px; }"
        "QPushButton:hover { background: #357ABD; }");

    auto* join_meeting_btn = new QPushButton("加入会议");
    join_meeting_btn->setStyleSheet(
        "QPushButton { background: #50C878; color: white; padding: 12px 24px; "
        "border-radius: 8px; font-size: 14px; }"
        "QPushButton:hover { background: #3DA85C; }");

    lobby_layout->addStretch();
    lobby_layout->addWidget(create_meeting_btn_, 0, Qt::AlignCenter);
    lobby_layout->addSpacing(16);
    lobby_layout->addWidget(join_meeting_btn, 0, Qt::AlignCenter);
    lobby_layout->addStretch();

    connect(create_meeting_btn_, &QPushButton::clicked,
            this, &MainWindow::on_create_meeting);
    connect(join_meeting_btn, &QPushButton::clicked, this, [this]() {
        bool ok;
        QString room_id = QInputDialog::getText(this, "加入会议",
            "输入房间号:", QLineEdit::Normal, "", &ok);
        if (ok && !room_id.isEmpty()) {
            on_join_meeting(room_id);
        }
    });

    stack_->addWidget(lobby_page_);

    // ── 会议页 ───────────────────────────────────────────
    meeting_page_ = new MeetingRoom(this);
    stack_->addWidget(meeting_page_);
}

void MainWindow::setup_navigation() {
    nav_bar_ = new QWidget(this);
    auto* nav_layout = new QHBoxLayout(nav_bar_);

    user_label_ = new QLabel("WeMeet");
    user_label_->setStyleSheet("color: #333333; font-size: 16px; font-weight: bold; background: transparent;");

    logout_btn_ = new QPushButton("退出");
    logout_btn_->setStyleSheet(
        "QPushButton { background: transparent; color: #FF6B6B; border: 1px solid #FF6B6B; "
        "padding: 6px 16px; border-radius: 4px; }"
        "QPushButton:hover { background: #FF6B6B; color: white; }");

    nav_layout->addWidget(user_label_);
    nav_layout->addStretch();
    nav_layout->addWidget(logout_btn_);

    nav_bar_->setStyleSheet("background: #ffffff; padding: 8px; border-bottom: 1px solid #e8edf5;");
    nav_bar_->setFixedHeight(48);
    nav_bar_->hide();

    connect(logout_btn_, &QPushButton::clicked, this, &MainWindow::on_logout);
}

// ── 槽函数 ───────────────────────────────────────────────
void MainWindow::on_login_success(uint64_t user_id, const QString& nickname) {
    current_user_id_ = user_id;
    user_label_->setText(nickname);
    nav_bar_->show();
    stack_->setCurrentIndex(PAGE_LOBBY);
}

void MainWindow::on_create_meeting() {
    stack_->setCurrentIndex(PAGE_MEETING);
    meeting_page_->set_room_info("meeting_" + QString::number(current_user_id_),
                                  "我的会议室");
}

void MainWindow::on_join_meeting(const QString& room_id) {
    stack_->setCurrentIndex(PAGE_MEETING);
    meeting_page_->set_room_info(room_id, "会议室: " + room_id);
}

void MainWindow::on_logout() {
    current_user_id_ = 0;
    nav_bar_->hide();
    stack_->setCurrentIndex(PAGE_LOBBY);
}
