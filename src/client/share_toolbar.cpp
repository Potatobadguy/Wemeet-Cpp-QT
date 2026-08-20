#include "share_toolbar.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QScreen>

FloatingShareToolbar::FloatingShareToolbar(QWidget* parent)
    : QFrame(parent) {
    // 无边框置顶工具窗（不出现在任务栏）
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setCursor(Qt::OpenHandCursor);
    setup_ui();
    set_state(ShareState::Sharing);
    adjustSize();

    // 默认停靠：主屏右上角
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        const QRect avail = s->availableGeometry();
        move(avail.right() - width() - 24, avail.top() + 24);
    }
}

void FloatingShareToolbar::setup_ui() {
    setStyleSheet(
        "FloatingShareToolbar { background: #1f2937; border: 1px solid #374151; "
        "border-radius: 10px; }"
        "QLabel { color: #e5e7eb; font-size: 13px; font-weight: bold; }"
        "QPushButton { color: white; border-radius: 6px; padding: 4px 14px; "
        "font-size: 12px; font-weight: bold; }");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(10);

    status_label_ = new QLabel(QStringLiteral("● 正在共享"), this);
    layout->addWidget(status_label_);

    pause_btn_ = new QPushButton(QStringLiteral("暂停"), this);
    pause_btn_->setStyleSheet("QPushButton { background: #4b5563; }"
                              "QPushButton:hover { background: #6b7280; }");
    layout->addWidget(pause_btn_);

    resume_btn_ = new QPushButton(QStringLiteral("恢复"), this);
    resume_btn_->setStyleSheet("QPushButton { background: #059669; }"
                               "QPushButton:hover { background: #10b981; }");
    layout->addWidget(resume_btn_);

    stop_btn_ = new QPushButton(QStringLiteral("停止共享"), this);
    stop_btn_->setStyleSheet("QPushButton { background: #dc2626; }"
                             "QPushButton:hover { background: #ef4444; }");
    layout->addWidget(stop_btn_);

    connect(pause_btn_,  &QPushButton::clicked, this, &FloatingShareToolbar::pause_clicked);
    connect(resume_btn_, &QPushButton::clicked, this, &FloatingShareToolbar::resume_clicked);
    connect(stop_btn_,   &QPushButton::clicked, this, &FloatingShareToolbar::stop_clicked);
}

void FloatingShareToolbar::set_state(ShareState state) {
    state_ = state;
    switch (state) {
    case ShareState::Sharing:
        status_label_->setText(QStringLiteral("● 正在共享"));
        status_label_->setStyleSheet("color: #34d399;");
        pause_btn_->setVisible(true);
        resume_btn_->setVisible(false);
        break;
    case ShareState::Paused:
        status_label_->setText(QStringLiteral("‖ 已暂停"));
        status_label_->setStyleSheet("color: #fbbf24;");
        pause_btn_->setVisible(false);
        resume_btn_->setVisible(true);
        break;
    case ShareState::Idle:
        hide();
        break;
    }
    adjustSize();
}

// ── 拖动实现 ────────────────────────────────────────────────

void FloatingShareToolbar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void FloatingShareToolbar::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - drag_offset_);
        event->accept();
    }
}

void FloatingShareToolbar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
    }
}
