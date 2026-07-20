#include "login_dialog.h"
#include <QMessageBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QSpacerItem>

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent) {

    setWindowTitle("WeMeet — 登录");
    setWindowFlags(Qt::FramelessWindowHint);
    setFixedWidth(640);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QDialog { background: transparent; border: none; }");

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName("loginCard");
    card->setStyleSheet(
        "QFrame#loginCard { background: white; border-radius: 24px; border: 1px solid #e8edf5; }"
        "QLabel { background: transparent; }");

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(48, 8, 48, 44);
    card_layout->setSpacing(0);

    // ── 标题栏（可拖动 + 关闭按钮）─────────────────────
    auto* title_bar = new QWidget(this);
    title_bar->setFixedHeight(40);
    title_bar_ = title_bar;
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(0, 4, 0, 4);
    title_layout->setSpacing(0);

    auto* title_label = new QLabel("WeMeet");
    title_label->setStyleSheet(
        "QLabel { color: #4A90D9; font-size: 14px; font-weight: bold; "
        "background: transparent; }");

    auto* close_btn = new QPushButton("\u00D7");  // × 关闭按钮
    close_btn->setFixedSize(36, 36);
    close_btn->setCursor(Qt::PointingHandCursor);
    close_btn->setStyleSheet(
        "QPushButton { background: transparent; color: #8896a6; "
        "border: none; border-radius: 18px; font-size: 22px; font-weight: 300; }"
        "QPushButton:hover { background: #f0f0f0; color: #333333; }"
        "QPushButton:pressed { background: #e0e0e0; color: #333333; }");
    close_btn->setToolTip("关闭");
    close_btn_ = close_btn;

    title_layout->addWidget(title_label);
    title_layout->addStretch();
    title_layout->addWidget(close_btn);

    card_layout->addWidget(title_bar);
    connect(close_btn, &QPushButton::clicked, this, &LoginDialog::on_close_clicked);

    // 让整个标题栏可拖动
    title_bar->setMouseTracking(true);
    title_bar->installEventFilter(this);
    close_btn->installEventFilter(this);

    // ── Logo ────────────────────────────────────────
    auto* logo = new QLabel("WeMeet");
    logo->setStyleSheet("QLabel { color: #4A90D9; font-size: 42px; font-weight: bold; "
                         "background: transparent; }");
    logo->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(logo);
    card_layout->addSpacing(8);

    auto* subtitle = new QLabel("企业级视频会议");
    subtitle->setStyleSheet("QLabel { color: #8896a6; font-size: 16px; "
                            "background: transparent; }");
    subtitle->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(subtitle);
    card_layout->addSpacing(24);

    // ── 标签页 ──────────────────────────────────────
    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: none; background: transparent; padding: 0px; margin: 0px; }"
        "QTabBar::tab { padding: 12px 60px; font-size: 16px; color: #8896a6; "
        "background: transparent; border: none; }"
        "QTabBar::tab:selected { color: #4A90D9; border-bottom: 2px solid #4A90D9; }"
        "QTabBar::tab:!selected { border-bottom: 2px solid transparent; }");

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
    layout->setLabelAlignment(Qt::AlignLeft);

    QString input_style =
        "QLineEdit { padding: 14px; font-size: 15px; border: 1px solid #e0e0e0; "
        "border-radius: 12px; background: #f8f9fa; color: #333333; }"
        "QLineEdit:focus { border-color: #4A90D9; background: #ffffff; }"
        "QLineEdit::placeholder { color: #b0b0b0; }";

    auto* email_label = new QLabel("邮箱");
    email_label->setStyleSheet("QLabel { color: #555555; font-size: 15px; "
                                "background: transparent; }");
    login_email_ = new QLineEdit();
    login_email_->setPlaceholderText("请输入邮箱");
    login_email_->setStyleSheet(input_style);
    login_email_->setMinimumHeight(46);
    layout->addRow(email_label, login_email_);

    auto* pass_label = new QLabel("密码");
    pass_label->setStyleSheet("QLabel { color: #555555; font-size: 15px; "
                              "background: transparent; }");
    login_pass_ = new QLineEdit();
    login_pass_->setPlaceholderText("请输入密码");
    login_pass_->setEchoMode(QLineEdit::Password);
    login_pass_->setStyleSheet(input_style);
    login_pass_->setMinimumHeight(46);
    layout->addRow(pass_label, login_pass_);

    login_status_ = new QLabel("");
    login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                 "background: transparent; }");
    login_status_->setAlignment(Qt::AlignCenter);
    login_status_->setMinimumHeight(20);
    layout->addRow(login_status_);

    login_btn_ = new QPushButton("登录");
    login_btn_->setCursor(Qt::PointingHandCursor);
    login_btn_->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 14px; "
        "border-radius: 12px; font-size: 17px; font-weight: bold; margin-top: 14px; }"
        "QPushButton:hover { background: #357ABD; }"
        "QPushButton:pressed { background: #2A5F9E; }"
        "QPushButton:disabled { background: #a0c4e8; }");
    login_btn_->setMinimumHeight(50);
    layout->addRow(login_btn_);

    connect(login_btn_, &QPushButton::clicked, this, &LoginDialog::on_login_clicked);
    connect(login_pass_, &QLineEdit::returnPressed, this, &LoginDialog::on_login_clicked);
}

void LoginDialog::setup_register_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(12);
    layout->setContentsMargins(0, 20, 0, 0);
    layout->setLabelAlignment(Qt::AlignLeft);

    QString input_style =
        "QLineEdit { padding: 12px; font-size: 14px; border: 1px solid #e0e0e0; "
        "border-radius: 10px; background: #f8f9fa; color: #333333; }"
        "QLineEdit:focus { border-color: #50C878; background: #ffffff; }"
        "QLineEdit::placeholder { color: #b0b0b0; }";

    QString label_style = "QLabel { color: #555555; font-size: 14px; "
                          "background: transparent; }";

    auto* email_label = new QLabel("邮箱");
    email_label->setStyleSheet(label_style);
    reg_email_ = new QLineEdit();
    reg_email_->setPlaceholderText("请输入邮箱（将作为登录账号）");
    reg_email_->setStyleSheet(input_style);
    reg_email_->setMinimumHeight(42);
    layout->addRow(email_label, reg_email_);

    auto* nick_label = new QLabel("昵称");
    nick_label->setStyleSheet(label_style);
    reg_nickname_ = new QLineEdit();
    reg_nickname_->setPlaceholderText("请输入显示昵称（可选，留空使用邮箱前缀）");
    reg_nickname_->setStyleSheet(input_style);
    reg_nickname_->setMinimumHeight(42);
    layout->addRow(nick_label, reg_nickname_);

    auto* pass_label = new QLabel("密码");
    pass_label->setStyleSheet(label_style);
    reg_pass_ = new QLineEdit();
    reg_pass_->setPlaceholderText("请输入密码（至少 6 位）");
    reg_pass_->setEchoMode(QLineEdit::Password);
    reg_pass_->setStyleSheet(input_style);
    reg_pass_->setMinimumHeight(42);
    layout->addRow(pass_label, reg_pass_);

    auto* pass2_label = new QLabel("确认密码");
    pass2_label->setStyleSheet(label_style);
    reg_pass2_ = new QLineEdit();
    reg_pass2_->setPlaceholderText("请再次输入密码");
    reg_pass2_->setEchoMode(QLineEdit::Password);
    reg_pass2_->setStyleSheet(input_style);
    reg_pass2_->setMinimumHeight(42);
    layout->addRow(pass2_label, reg_pass2_);

    reg_status_ = new QLabel("");
    reg_status_->setStyleSheet("QLabel { color: #8896a6; font-size: 13px; "
                               "background: transparent; }");
    reg_status_->setAlignment(Qt::AlignCenter);
    reg_status_->setMinimumHeight(20);
    layout->addRow(reg_status_);

    reg_btn_ = new QPushButton("立即注册");
    reg_btn_->setCursor(Qt::PointingHandCursor);
    reg_btn_->setStyleSheet(
        "QPushButton { background: #50C878; color: white; padding: 12px; "
        "border-radius: 10px; font-size: 16px; font-weight: bold; margin-top: 10px; }"
        "QPushButton:hover { background: #45B070; }"
        "QPushButton:pressed { background: #3A9A60; }"
        "QPushButton:disabled { background: #a8d8c0; }");
    reg_btn_->setMinimumHeight(46);
    reg_btn_->setEnabled(true);
    layout->addRow(reg_btn_);

    connect(reg_btn_, &QPushButton::clicked, this, &LoginDialog::on_register_clicked);
    connect(reg_pass2_, &QLineEdit::returnPressed, this, &LoginDialog::on_register_clicked);
}

void LoginDialog::on_login_clicked() {
    QString email = login_email_->text().trimmed();
    QString pass  = login_pass_->text();
    if (email.isEmpty() || pass.isEmpty()) {
        login_status_->setText("请输入邮箱和密码");
        login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                     "background: transparent; }");
        login_status_->show();
        return;
    }
    login_btn_->setEnabled(false);
    login_btn_->setText("正在验证...");
    login_status_->setText("正在验证...");
    login_status_->setStyleSheet("QLabel { color: #888; font-size: 13px; "
                                 "background: transparent; }");
    login_status_->show();

    // 发出信号，由 MainWindow 通过网络验证
    emit login_request(email, pass);
}

void LoginDialog::show_error(const QString& msg) {
    login_btn_->setEnabled(true);
    login_btn_->setText("登录");
    login_status_->setText(msg);
    login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                 "background: transparent; }");
    login_status_->show();
}

void LoginDialog::show_register_result(bool success, const QString& msg) {
    reg_btn_->setEnabled(true);
    reg_btn_->setText("立即注册");
    if (success) {
        reg_status_->setText("\u2713 " + msg + " 正在为您登录...");
        reg_status_->setStyleSheet("QLabel { color: #50C878; font-size: 13px; "
                                   "background: transparent; }");
        // 切换到登录标签页并填充邮箱
        QList<QTabWidget*> tabs = findChildren<QTabWidget*>();
        if (!tabs.isEmpty()) {
            tabs[0]->setCurrentIndex(0);
        }
        if (reg_email_) {
            login_email_->setText(reg_email_->text());
        }
        // 清空注册表单
        reg_email_->clear();
        reg_nickname_->clear();
        reg_pass_->clear();
        reg_pass2_->clear();
    } else {
        reg_status_->setText("\u2717 " + msg);
        reg_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                   "background: transparent; }");
    }
}

void LoginDialog::on_register_clicked() {
    QString email = reg_email_->text().trimmed();
    QString pass  = reg_pass_->text();
    QString pass2 = reg_pass2_->text();
    QString nick  = reg_nickname_->text().trimmed();

    // 校验
    if (email.isEmpty()) {
        reg_status_->setText("请输入邮箱");
        reg_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                   "background: transparent; }");
        return;
    }
    if (pass.length() < 6) {
        reg_status_->setText("密码至少需要 6 位");
        reg_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                   "background: transparent; }");
        return;
    }
    if (pass != pass2) {
        reg_status_->setText("两次输入的密码不一致");
        reg_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                   "background: transparent; }");
        return;
    }

    // 简单邮箱格式校验
    if (!email.contains('@') || !email.contains('.')) {
        reg_status_->setText("邮箱格式不正确（需要包含 @ 和 .）");
        reg_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; "
                                   "background: transparent; }");
        return;
    }

    // 昵称为空时使用邮箱前缀
    if (nick.isEmpty()) {
        nick = email.left(email.indexOf('@'));
        if (nick.isEmpty()) nick = email;
    }

    reg_btn_->setEnabled(false);
    reg_btn_->setText("正在注册...");
    reg_status_->setText("正在向服务器提交...");
    reg_status_->setStyleSheet("QLabel { color: #888; font-size: 13px; "
                               "background: transparent; }");

    // 发出信号，由 MainWindow 通过网络注册
    emit register_request(email, pass, nick);
}

void LoginDialog::on_close_clicked() {
    reject();
}

// ── 窗口拖动事件 ──────────────────────────────────────────
bool LoginDialog::eventFilter(QObject* obj, QEvent* event) {
    if (!title_bar_) return QDialog::eventFilter(obj, event);
    // 标题栏和其子控件都可拖动
    QWidget* w = qobject_cast<QWidget*>(obj);
    bool in_title_bar = (w == title_bar_ ||
                         (w && title_bar_->isAncestorOf(w)));
    if (in_title_bar && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            // 排除关闭按钮的点击
            if (w != close_btn_) {
                drag_pos_ = me->globalPosition().toPoint() - frameGeometry().topLeft();
                dragging_ = true;
            }
        }
    }
    if (in_title_bar && event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (dragging_ && (me->buttons() & Qt::LeftButton))
            move(me->globalPosition().toPoint() - drag_pos_);
    }
    if (in_title_bar && event->type() == QEvent::MouseButtonRelease) {
        dragging_ = false;
    }
    return QDialog::eventFilter(obj, event);
}

void LoginDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        drag_pos_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
    QDialog::mousePressEvent(event);
}

void LoginDialog::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && (event->buttons() & Qt::LeftButton))
        move(event->globalPosition().toPoint() - drag_pos_);
    QDialog::mouseMoveEvent(event);
}

void LoginDialog::mouseReleaseEvent(QMouseEvent* event) {
    dragging_ = false;
    QDialog::mouseReleaseEvent(event);
}
