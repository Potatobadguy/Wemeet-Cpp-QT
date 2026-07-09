#include "login_dialog.h"
#include <QMessageBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QFrame>

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent) {

    setWindowTitle("WeMeet — 登录");
    setWindowFlags(Qt::FramelessWindowHint);    // 去掉系统标题栏
    setFixedWidth(640);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);


    // 透明窗口 + 内部白色卡片铺满整个窗口，实现圆角且无镂空
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QDialog { background: transparent; border: none; }");

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // 白色圆角卡片，铺满整个对话框
    auto* card = new QFrame(this);
    card->setObjectName("loginCard");
    card->setStyleSheet(
        "QFrame#loginCard {"
        "  background: white;"
        "  border-radius: 24px;"
        "  border: 1px solid #e8edf5;"
        "}"
        "QLabel { background: transparent; }");

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(48, 0, 48, 44);
    card_layout->setSpacing(0);

    // 自定义标题栏（集成到卡片顶部）
    auto* title_bar = new QWidget(this);
    title_bar_ = title_bar;
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(0, 12, 0, 12);
    title_layout->setSpacing(0);

    auto* title_label = new QLabel("WeMeet");
    title_label->setStyleSheet(
        "QLabel { color: #4A90D9; font-size: 14px; font-weight: bold; }");

    auto* close_btn = new QPushButton("×");
    close_btn->setFixedSize(32, 32);
    close_btn->setStyleSheet(
        "QPushButton { background: transparent; color: #8896a6; "
        "border: none; border-radius: 16px; font-size: 20px; font-weight: bold; }"
        "QPushButton:hover { background: #f0f0f0; color: #333333; }");

    title_layout->addWidget(title_label);
    title_layout->addStretch();
    title_layout->addWidget(close_btn);

    card_layout->addWidget(title_bar);

    connect(close_btn, &QPushButton::clicked, this, &LoginDialog::on_close_clicked);

    // 内容区可拖动
    title_bar->setMouseTracking(true);
    title_bar->installEventFilter(this);

    // Logo
    auto* logo = new QLabel("WeMeet");
    logo->setStyleSheet(
        "QLabel { color: #4A90D9; font-size: 42px; font-weight: bold; }");
    logo->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(logo);
    card_layout->addSpacing(8);

    auto* subtitle = new QLabel("企业级视频会议");
    subtitle->setStyleSheet(
        "QLabel { color: #8896a6; font-size: 16px; }");
    subtitle->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(subtitle);
    card_layout->addSpacing(36);

    // 选项卡: 登录 | 注册
    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: none; background: transparent; padding: 0px; margin: 0px; }"
        "QTabBar::tab { padding: 12px 60px; font-size: 16px; "
        "color: #8896a6; background: transparent; border: none; }"
        "QTabBar::tab:selected { color: #4A90D9; border-bottom: 2px solid #4A90D9; }"
        "QTabBar::tab:!selected { border-bottom: 2px solid transparent; }"
        "QTabWidget::tab-bar { background: transparent; }");

    auto* login_tab = new QWidget();
    setup_login_tab(login_tab);
    tabs->addTab(login_tab, "登录");

    auto* reg_tab = new QWidget();
    setup_register_tab(reg_tab);
    tabs->addTab(reg_tab, "注册");

    card_layout->addWidget(tabs);
    main_layout->addWidget(card);
}

void LoginDialog::setup_login_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(0, 20, 0, 0);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QString label_style =
        "QLabel { color: #555555; font-size: 15px; }";
    QString input_style =
        "QLineEdit { padding: 14px; font-size: 15px; border: 1px solid #e0e0e0; "
        "border-radius: 12px; background: #f8f9fa; color: #333333; }"
        "QLineEdit:focus { border-color: #4A90D9; background: #ffffff; }"
        "QLineEdit::placeholder { color: #b0b0b0; }";

    auto* email_label = new QLabel("邮箱");
    email_label->setStyleSheet(label_style);
    login_email_ = new QLineEdit();
    login_email_->setPlaceholderText("请输入邮箱");
    login_email_->setStyleSheet(input_style);
    login_email_->setMinimumHeight(46);
    layout->addRow(email_label, login_email_);

    auto* pass_label = new QLabel("密码");
    pass_label->setStyleSheet(label_style);
    login_pass_ = new QLineEdit();
    login_pass_->setPlaceholderText("请输入密码");
    login_pass_->setEchoMode(QLineEdit::Password);
    login_pass_->setStyleSheet(input_style);
    login_pass_->setMinimumHeight(46);
    layout->addRow(pass_label, login_pass_);

    auto* login_btn = new QPushButton("登录");
    login_btn->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 14px; "
        "border-radius: 12px; font-size: 17px; font-weight: bold; margin-top: 14px; }"
        "QPushButton:hover { background: #357ABD; }"
        "QPushButton:pressed { background: #2a6aa8; }");
    login_btn->setMinimumHeight(50);
    layout->addRow(login_btn);

    connect(login_btn, &QPushButton::clicked, this, &LoginDialog::on_login_clicked);
}

void LoginDialog::setup_register_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(0, 20, 0, 0);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QString label_style =
        "QLabel { color: #555555; font-size: 15px; }";
    QString input_style =
        "QLineEdit { padding: 14px; font-size: 15px; border: 1px solid #e0e0e0; "
        "border-radius: 12px; background: #f8f9fa; color: #333333; }"
        "QLineEdit:focus { border-color: #50C878; background: #ffffff; }"
        "QLineEdit::placeholder { color: #b0b0b0; }";

    auto* email_label = new QLabel("邮箱");
    email_label->setStyleSheet(label_style);
    reg_email_ = new QLineEdit();
    reg_email_->setPlaceholderText("请输入邮箱");
    reg_email_->setStyleSheet(input_style);
    reg_email_->setMinimumHeight(46);
    layout->addRow(email_label, reg_email_);

    auto* nickname_label = new QLabel("昵称");
    nickname_label->setStyleSheet(label_style);
    reg_nickname_ = new QLineEdit();
    reg_nickname_->setPlaceholderText("请输入昵称");
    reg_nickname_->setStyleSheet(input_style);
    reg_nickname_->setMinimumHeight(46);
    layout->addRow(nickname_label, reg_nickname_);

    auto* pass_label = new QLabel("密码");
    pass_label->setStyleSheet(label_style);
    reg_pass_ = new QLineEdit();
    reg_pass_->setPlaceholderText("请输入密码（至少6位）");
    reg_pass_->setEchoMode(QLineEdit::Password);
    reg_pass_->setStyleSheet(input_style);
    reg_pass_->setMinimumHeight(46);
    layout->addRow(pass_label, reg_pass_);

    auto* confirm_label = new QLabel("确认密码");
    confirm_label->setStyleSheet(label_style);
    reg_confirm_pass_ = new QLineEdit();
    reg_confirm_pass_->setPlaceholderText("请确认密码");
    reg_confirm_pass_->setEchoMode(QLineEdit::Password);
    reg_confirm_pass_->setStyleSheet(input_style);
    reg_confirm_pass_->setMinimumHeight(46);
    layout->addRow(confirm_label, reg_confirm_pass_);

    auto* reg_btn = new QPushButton("注册");
    reg_btn->setStyleSheet(
        "QPushButton { background: #50C878; color: white; padding: 14px; "
        "border-radius: 12px; font-size: 17px; font-weight: bold; margin-top: 14px; }"
        "QPushButton:hover { background: #3DA85C; }"
        "QPushButton:pressed { background: #2f8a4c; }");
    reg_btn->setMinimumHeight(50);
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
    hide();   // 隐藏登录窗口，不阻塞主窗口显示
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

void LoginDialog::on_close_clicked() {
    reject();
}

bool LoginDialog::eventFilter(QObject* obj, QEvent* event) {
    if (obj == title_bar_ && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);
        if (mouse_event->button() == Qt::LeftButton) {
            drag_pos_ = mouse_event->globalPosition().toPoint() - frameGeometry().topLeft();
            dragging_ = true;
        }
    }
    if (obj == title_bar_ && event->type() == QEvent::MouseMove) {
        QMouseEvent* mouse_event = static_cast<QMouseEvent*>(event);
        if (dragging_ && (mouse_event->buttons() & Qt::LeftButton)) {
            move(mouse_event->globalPosition().toPoint() - drag_pos_);
        }
    }
    if (obj == title_bar_ && event->type() == QEvent::MouseButtonRelease) {
        dragging_ = false;
    }
    return QDialog::eventFilter(obj, event);
}

void LoginDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        drag_pos_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        dragging_ = true;
    }
    QDialog::mousePressEvent(event);
}

void LoginDialog::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - drag_pos_);
    }
    QDialog::mouseMoveEvent(event);
}

void LoginDialog::mouseReleaseEvent(QMouseEvent* event) {
    dragging_ = false;
    QDialog::mouseReleaseEvent(event);
}
