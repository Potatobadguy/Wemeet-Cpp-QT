#pragma once
/**
 * @file share_toolbar.h
 * @brief 屏幕共享浮动工具条（#23）
 *
 * 可拖动悬浮窗（Qt::Tool + FramelessWindowHint + WindowStaysOnTopHint）：
 *   - 共享中：显示「● 正在共享」（绿）+ [暂停] [停止] 按钮；
 *   - 已暂停：显示「‖ 已暂停」（橙）+ [恢复] [停止] 按钮；
 *   - 鼠标左键按住空白处可拖动到屏幕任意位置。
 */

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QPoint>
#include "screen_share_controller.h"   // ShareState

class FloatingShareToolbar : public QFrame {
    Q_OBJECT
public:
    explicit FloatingShareToolbar(QWidget* parent = nullptr);
    ~FloatingShareToolbar() override = default;

    /**
     * @brief 同步共享状态（切换文案与按钮显隐）
     */
    void set_state(ShareState state);
    ShareState state() const { return state_; }

signals:
    void pause_clicked();
    void resume_clicked();
    void stop_clicked();

protected:
    // 拖动支持
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void setup_ui();

    ShareState   state_ = ShareState::Idle;
    QLabel*      status_label_ = nullptr;
    QPushButton* pause_btn_ = nullptr;    // 共享中显示「暂停」
    QPushButton* resume_btn_ = nullptr;   // 暂停时显示「恢复」
    QPushButton* stop_btn_ = nullptr;

    bool  dragging_ = false;
    QPoint drag_offset_;   // 按下点相对窗口左上角的偏移
};
