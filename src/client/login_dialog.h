#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QTabWidget>

/**
 * @brief 登录/注册对话框 — 信号槽发送 login_success 信号
 */
class LoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QWidget* parent = nullptr);

signals:
    void login_success(uint64_t user_id, const QString& nickname);

private slots:
    void on_login_clicked();
    void on_register_clicked();

private:
    void setup_login_tab(QWidget* tab);
    void setup_register_tab(QWidget* tab);

    QLineEdit* login_email_;
    QLineEdit* login_pass_;

    QLineEdit* reg_email_;
    QLineEdit* reg_pass_;
    QLineEdit* reg_nickname_;
    QLineEdit* reg_confirm_pass_;
};
