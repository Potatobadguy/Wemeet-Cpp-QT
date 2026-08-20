#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QMouseEvent>
#include <QPoint>

/**
 * @brief 登录/注册对话框 — 只采集凭据，网络认证由 MainWindow 完成
 */
class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);

    void show_error(const QString& msg);
    void show_register_result(bool success, const QString& msg);

    // 调试便利：填充上次保存的凭据并可选勾选"记住密码"
    void prefill_credentials(const QString& email, const QString& password,
                            bool remember_password);

signals:
    void login_request(const QString& email, const QString& password);
    void register_request(const QString& email, const QString& password,
                          const QString& nickname);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void on_login_clicked();
    void on_register_clicked();
    void on_close_clicked();

private:
    void setup_login_tab(QWidget* tab);
    void setup_register_tab(QWidget* tab);

    // ── 登录字段 ──
public:
    QLineEdit* login_email_ = nullptr;
    QLineEdit* login_pass_  = nullptr;
    QCheckBox* login_remember_pass_ = nullptr;   // 记住密码
private:
    QPushButton* login_btn_ = nullptr;
    QLabel*     login_status_ = nullptr;

    // ── 注册字段 ──
    QLineEdit* reg_email_    = nullptr;
    QLineEdit* reg_nickname_ = nullptr;
    QLineEdit* reg_pass_     = nullptr;
    QLineEdit* reg_pass2_    = nullptr;
    QPushButton* reg_btn_    = nullptr;
    QLabel*     reg_status_  = nullptr;

    // ── 拖动与标题栏 ──
    QWidget* title_bar_ = nullptr;
    QPushButton* close_btn_ = nullptr;
    QPoint   drag_pos_;
    bool     dragging_ = false;
};
