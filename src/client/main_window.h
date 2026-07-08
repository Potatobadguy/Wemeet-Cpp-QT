#pragma once
#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <memory>

class LoginDialog;
class MeetingRoom;
class NetworkClient;

/**
 * @brief 主窗口 — 信号槽驱动的页面切换
 *
 * 技术亮点：
 *   - QStackedWidget 实现多页面无闪烁切换
 *   - 信号槽连接网络事件与 UI 更新
 *   - 自定义导航栏样式
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void on_login_success(uint64_t user_id, const QString& nickname);
    void on_create_meeting();
    void on_join_meeting(const QString& room_id);
    void on_logout();

private:
    void setup_ui();
    void setup_navigation();

    // ── 页面 ─────────────────────────────────────────────
    enum Page { PAGE_LOGIN = 0, PAGE_LOBBY = 1, PAGE_MEETING = 2 };

    QStackedWidget* stack_;
    LoginDialog*    login_page_;
    QWidget*        lobby_page_;
    MeetingRoom*    meeting_page_;

    // ── 导航栏 ───────────────────────────────────────────
    QWidget*    nav_bar_;
    QLabel*     user_label_;
    QPushButton* create_meeting_btn_;
    QPushButton* logout_btn_;

    // ── 网络 ─────────────────────────────────────────────
    NetworkClient* network_;

    // ── 状态 ─────────────────────────────────────────────
    uint64_t current_user_id_ = 0;
};
