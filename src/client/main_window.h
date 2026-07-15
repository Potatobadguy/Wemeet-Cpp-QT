#pragma once
#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStack>
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
    void on_back_requested();

private:
    void setup_ui();
    void setup_navigation();
    void navigate_to(int page);   // 带历史记录的页面跳转
    void go_back();                // 返回上一页（无历史时回大厅）

    // ── 页面 ─────────────────────────────────────────────
    enum Page { PAGE_LOBBY = 0, PAGE_MEETING = 1 };


    QStackedWidget* stack_;
    QWidget*        lobby_page_;
    MeetingRoom*    meeting_page_;

    // ── 导航栏 ───────────────────────────────────────────
    QWidget*    nav_bar_;
    QLabel*     user_label_;
    QPushButton* back_btn_;        // 顶部返回按钮
    QPushButton* create_meeting_btn_;
    QPushButton* logout_btn_;

    // ── 网络 ─────────────────────────────────────────────
    NetworkClient* network_;

    // ── 状态 ─────────────────────────────────────────────
    uint64_t current_user_id_ = 0;
    QStack<int> nav_history_;   // 页面导航历史栈
};
