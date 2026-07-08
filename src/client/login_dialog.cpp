#include "login_dialog.h"
#include <QMessageBox>
#include <QFormLayout>
#include <QFrame>

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent) {

    setWindowTitle("WeMeet — 登录");
    setFixedSize(420, 520);
    setStyleSheet("QDialog { background: #1a1a2e; }");

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(40, 40, 40, 40);

    // Logo
    auto* logo = new QLabel("WeMeet");
    logo->setStyleSheet("color: #4A90D9; font-size: 32px; font-weight: bold;");
    logo->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(logo);
    main_layout->addSpacing(8);

    auto* subtitle = new QLabel("企业级视频会议");
    subtitle->setStyleSheet("color: #888; font-size: 12px;");
    subtitle->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(subtitle);
    main_layout->addSpacing(24);

    // 选项卡: 登录 | 注册
    auto* tabs = new QTabWidget(this);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #333; border-radius: 8px; background: #16213e; }"
        "QTabBar::tab { padding: 10px 40px; font-size: 14px; "
        "color: #888; background: transparent; }"
        "QTabBar::tab:selected { color: #4A90D9; border-bottom: 2px solid #4A90D9; }");

    auto* login_tab = new QWidget();
    setup_login_tab(login_tab);
    tabs->addTab(login_tab, "登录");

    auto* reg_tab = new QWidget();
    setup_register_tab(reg_tab);
    tabs->addTab(reg_tab, "注册");

    main_layout->addWidget(tabs);
}

void LoginDialog::setup_login_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(20, 30, 20, 20);

    QString input_style =
        "QLineEdit { padding: 10px; font-size: 14px; border: 1px solid #444; "
        "border-radius: 6px; background: #0f3460; color: #eee; }"
        "QLineEdit:focus { border-color: #4A90D9; }";

    login_email_ = new QLineEdit();
    login_email_->setPlaceholderText("请输入邮箱");
    login_email_->setStyleSheet(input_style);
    layout->addRow("邮箱:", login_email_);

    login_pass_ = new QLineEdit();
    login_pass_->setPlaceholderText("请输入密码");
    login_pass_->setEchoMode(QLineEdit::Password);
    login_pass_->setStyleSheet(input_style);
    layout->addRow("密码:", login_pass_);

    auto* login_btn = new QPushButton("登录");
    login_btn->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 12px; "
        "border-radius: 8px; font-size: 15px; font-weight: bold; margin-top: 10px; }"
        "QPushButton:hover { background: #357ABD; }");
    layout->addRow(login_btn);

    connect(login_btn, &QPushButton::clicked, this, &LoginDialog::on_login_clicked);
}

void LoginDialog::setup_register_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(20, 30, 20, 20);

    QString input_style =
        "QLineEdit { padding: 10px; font-size: 14px; border: 1px solid #444; "
        "border-radius: 6px; background: #0f3460; color: #eee; }"
        "QLineEdit:focus { border-color: #50C878; }";

    reg_email_ = new QLineEdit();
    reg_email_->setPlaceholderText("请输入邮箱");
    reg_email_->setStyleSheet(input_style);
    layout->addRow("邮箱:", reg_email_);

    reg_nickname_ = new QLineEdit();
    reg_nickname_->setPlaceholderText("请输入昵称");
    reg_nickname_->setStyleSheet(input_style);
    layout->addRow("昵称:", reg_nickname_);

    reg_pass_ = new QLineEdit();
    reg_pass_->setPlaceholderText("请输入密码（至少6位）");
    reg_pass_->setEchoMode(QLineEdit::Password);
    reg_pass_->setStyleSheet(input_style);
    layout->addRow("密码:", reg_pass_);

    reg_confirm_pass_ = new QLineEdit();
    reg_confirm_pass_->setPlaceholderText("请确认密码");
    reg_confirm_pass_->setEchoMode(QLineEdit::Password);
    reg_confirm_pass_->setStyleSheet(input_style);
    layout->addRow("确认密码:", reg_confirm_pass_);

    auto* reg_btn = new QPushButton("注册");
    reg_btn->setStyleSheet(
        "QPushButton { background: #50C878; color: white; padding: 12px; "
        "border-radius: 8px; font-size: 15px; font-weight: bold; margin-top: 10px; }"
        "QPushButton:hover { background: #3DA85C; }");
    layout->addRow(reg_btn);

    connect(reg_btn, &QPushButton::clicked, this, &LoginDialog::on_register_clicked);
}

void LoginDialog::on_login_clicked() {
    QString email = login_email_->text().trimmed();
    QString pass  = login_pass_->text();

    if (email.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入邮箱和密码");
        return;
    }

    // Mock: 演示用模拟登录成功
    emit login_success(1, email.split("@").first());
    accept();
}

void LoginDialog::on_register_clicked() {
    QString email    = reg_email_->text().trimmed();
    QString nickname = reg_nickname_->text().trimmed();
    QString pass     = reg_pass_->text();
    QString confirm  = reg_confirm_pass_->text();

    if (email.isEmpty() || nickname.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, "提示", "请填写所有字段");
        return;
    }
    if (pass != confirm) {
        QMessageBox::warning(this, "提示", "两次密码不一致");
        return;
    }
    if (pass.length() < 6) {
        QMessageBox::warning(this, "提示", "密码至少6位");
        return;
    }

    QMessageBox::information(this, "成功", "注册成功！请登录");
}
