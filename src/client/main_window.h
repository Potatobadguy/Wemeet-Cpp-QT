#pragma once
#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStack>
#include <QString>
#include <memory>

class LoginDialog;
class MeetingRoom;
class NetworkClient;
class MediaEngine;
class QMessageBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    // 公开接口（供 main.cpp 调用）
    void on_login_success(uint64_t user_id, const QString& nickname);
    void login_via_server(const QString& email, const QString& password);
    void register_via_server(const QString& email, const QString& password,
                             const QString& nickname);
    bool connect_to_server();
    QString server_host() const { return server_host_; }
    uint16_t server_port() const { return server_port_; }

    // 认证信号
signals:
    void auth_success(uint64_t user_id, const QString& nickname);
    void auth_failed(const QString& error_msg);
    void register_result(bool success, const QString& msg);

private slots:
    void on_create_meeting();
    void on_join_meeting(const QString& room_id);
    void on_logout();
    void on_back_requested();

    // 网络事件
    void on_network_connected();
    void on_network_disconnected();
    void on_network_error(const QString& err);
    void on_network_message(const std::string& data);

    // 媒体引擎相关
    void on_network_quality_changed(int32_t quality);

private:
    void setup_ui();
    void setup_navigation();
    void navigate_to(int page);
    void go_back();
    void update_back_button();

    // 初始化媒体引擎
    void init_media_engine(uint64_t user_id, const QString& room_id,
                           const QString& relay_host, uint16_t relay_port);

    // 服务器交互
    void send_create_meeting();
    void send_join_meeting(const QString& room_id);
    void send_chat(const QString& content);
    void send_leave_meeting();
    void send_logout();
    void enter_meeting(const QString& room_id);
    void process_incoming_message(const std::string& data);
    void handle_participant_update(const std::string& data);
    void handle_chat_receive(const std::string& data);

    // ── 页面 ─────────────────────────────────────────────
    enum Page { PAGE_LOBBY = 0, PAGE_MEETING = 1 };

    QStackedWidget* stack_;
    QWidget*        lobby_page_;
    MeetingRoom*    meeting_page_;

    // ── 导航栏 ───────────────────────────────────────────
    QWidget*    nav_bar_;
    QLabel*     user_label_;
    QLabel*     server_status_label_;   // 服务器连接状态
    QPushButton* back_btn_;
    QPushButton* create_meeting_btn_;
    QPushButton* logout_btn_;

    // ── 网络 ─────────────────────────────────────────────
    NetworkClient* network_;

    // ── 媒体 ─────────────────────────────────────────────
    MediaEngine* media_engine_ = nullptr;

    // ── 状态 ─────────────────────────────────────────────
    uint64_t current_user_id_ = 0;
    QString  current_nickname_;
    QString  current_room_id_;
    bool     in_meeting_ = false;
    bool     pending_create_ = false;   // 等待服务器 CREATE_RESP
    bool     pending_join_   = false;   // 等待服务器 JOIN_RESP
    QString  pending_join_room_id_;     // 等待加入的 room_id
    bool     waiting_login_ = false;    // 等待 LOGIN_RESP
    bool     login_success_ = false;    // 登录结果
    bool     waiting_register_ = false;  // 等待 REGISTER_RESP
    QStack<int> nav_history_;

    // 服务器配置
    QString server_host_ = "127.0.0.1";
    uint16_t server_port_ = 9090;
};
