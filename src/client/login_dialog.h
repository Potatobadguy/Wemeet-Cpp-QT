#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QMouseEvent>
#include <QTcpSocket>
#include <QTimer>

/**
 * @brief 登录/注册对话框 — 通过服务器验证账号密码
 *
 * 流程：
 *   用户点击登录 → QTcpSocket 连接服务器 → 发送 LOGIN_REQ
 *   → 等待 LOGIN_RESP → 成功 emit login_success / 失败显示错误
 */
class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);

    void set_server(const QString& host, uint16_t port);

signals:
    void login_success(uint64_t user_id, const QString& nickname);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void on_login_clicked();
    void on_register_clicked();
    void on_close_clicked();

    // 网络事件
    void on_socket_connected();
    void on_socket_ready_read();
    void on_socket_error(QAbstractSocket::SocketError err);
    void on_login_timeout();

private:
    void setup_login_tab(QWidget* tab);
    void setup_register_tab(QWidget* tab);
    void send_login_request(const QString& email, const QString& password);
    void send_register_request(const QString& email, const QString& password,
                                const QString& nickname);
    void set_controls_enabled(bool enabled);

    // 网络
    QTcpSocket* sock_ = nullptr;
    QByteArray  recv_buf_;
    QTimer*     timeout_timer_ = nullptr;
    QString     server_host_ = "127.0.0.1";
    uint16_t    server_port_ = 9090;

    // 状态
    bool waiting_for_response_ = false;

    // UI
    QLineEdit* login_email_;
    QLineEdit* login_pass_;
    QPushButton* login_btn_;
    QLabel*     login_status_;

    QLineEdit* reg_email_;
    QLineEdit* reg_pass_;
    QLineEdit* reg_nickname_;
    QLineEdit* reg_confirm_pass_;
    QPushButton* reg_btn_;

    QWidget* title_bar_ = nullptr;
    QPoint drag_pos_;
    bool   dragging_ = false;
};
