#include "login_dialog.h"
#include <QMessageBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QtEndian>

// Protobuf 消息
#include "common.pb.h"
#include "auth.pb.h"

// ── 辅助：序列化 BaseMessage + 4 字节长度头 ─────────────────

static QByteArray pack_message(int msg_type, uint64_t seq_id,
                                const google::protobuf::Message& payload) {
    std::string payload_bytes;
    payload.SerializeToString(&payload_bytes);

    wemeet::BaseMessage base;
    base.set_type(static_cast<wemeet::MsgType>(msg_type));
    base.set_sequence_id(seq_id);
    base.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    base.set_payload(payload_bytes);

    std::string base_bytes;
    base.SerializeToString(&base_bytes);

    uint32_t len = static_cast<uint32_t>(base_bytes.size());
    QByteArray out;
    out.resize(4 + len);
    uint32_t net_len = qToBigEndian(len);
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, base_bytes.data(), len);
    return out;
}

// ── 构造 ─────────────────────────────────────────────────────

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent) {

    setWindowTitle("WeMeet — 登录");
    setWindowFlags(Qt::FramelessWindowHint);
    setFixedWidth(640);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet("QDialog { background: transparent; border: none; }");

    // 网络
    sock_ = new QTcpSocket(this);
    recv_buf_.reserve(65536);
    connect(sock_, &QTcpSocket::connected, this, &LoginDialog::on_socket_connected);
    connect(sock_, &QTcpSocket::readyRead, this, &LoginDialog::on_socket_ready_read);
    connect(sock_, &QTcpSocket::errorOccurred, this, &LoginDialog::on_socket_error);

    // 超时定时器
    timeout_timer_ = new QTimer(this);
    timeout_timer_->setSingleShot(true);
    connect(timeout_timer_, &QTimer::timeout, this, &LoginDialog::on_login_timeout);

    // ── UI ───────────────────────────────────────────────
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

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

    auto* title_bar = new QWidget(this);
    title_bar_ = title_bar;
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(0, 12, 0, 12);

    auto* title_label = new QLabel("WeMeet");
    title_label->setStyleSheet("QLabel { color: #4A90D9; font-size: 14px; font-weight: bold; }");

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

    title_bar->setMouseTracking(true);
    title_bar->installEventFilter(this);

    auto* logo = new QLabel("WeMeet");
    logo->setStyleSheet("QLabel { color: #4A90D9; font-size: 42px; font-weight: bold; }");
    logo->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(logo);
    card_layout->addSpacing(8);

    auto* subtitle = new QLabel("企业级视频会议");
    subtitle->setStyleSheet("QLabel { color: #8896a6; font-size: 16px; }");
    subtitle->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(subtitle);
    card_layout->addSpacing(36);

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

void LoginDialog::set_server(const QString& host, uint16_t port) {
    server_host_ = host;
    server_port_ = port;
}

// ── UI 构建 ─────────────────────────────────────────────────

void LoginDialog::setup_login_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(0, 20, 0, 0);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    QString label_style = "QLabel { color: #555555; font-size: 15px; }";
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

    // 状态标签（连接中、错误提示）
    login_status_ = new QLabel("");
    login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; }");
    login_status_->setAlignment(Qt::AlignCenter);
    login_status_->hide();
    layout->addRow(login_status_);

    login_btn_ = new QPushButton("登录");
    login_btn_->setStyleSheet(
        "QPushButton { background: #4A90D9; color: white; padding: 14px; "
        "border-radius: 12px; font-size: 17px; font-weight: bold; margin-top: 14px; }"
        "QPushButton:hover { background: #357ABD; }"
        "QPushButton:pressed { background: #2a6aa8; }");
    login_btn_->setMinimumHeight(50);
    layout->addRow(login_btn_);

    connect(login_btn_, &QPushButton::clicked, this, &LoginDialog::on_login_clicked);

    // Enter 键触发登录
    connect(login_pass_, &QLineEdit::returnPressed, this, &LoginDialog::on_login_clicked);
}

void LoginDialog::setup_register_tab(QWidget* tab) {
    auto* layout = new QFormLayout(tab);
    layout->setSpacing(16);
    layout->setContentsMargins(0, 20, 0, 0);

    QString label_style = "QLabel { color: #555555; font-size: 15px; }";
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

    reg_btn_ = new QPushButton("注册");
    reg_btn_->setStyleSheet(
        "QPushButton { background: #50C878; color: white; padding: 14px; "
        "border-radius: 12px; font-size: 17px; font-weight: bold; margin-top: 14px; }"
        "QPushButton:hover { background: #3DA85C; }"
        "QPushButton:pressed { background: #2f8a4c; }");
    reg_btn_->setMinimumHeight(50);
    layout->addRow(reg_btn_);

    connect(reg_btn_, &QPushButton::clicked, this, &LoginDialog::on_register_clicked);
}

// ── 控件启用/禁用 ──────────────────────────────────────────

void LoginDialog::set_controls_enabled(bool enabled) {
    login_email_->setEnabled(enabled);
    login_pass_->setEnabled(enabled);
    login_btn_->setEnabled(enabled);
    reg_email_->setEnabled(enabled);
    reg_nickname_->setEnabled(enabled);
    reg_pass_->setEnabled(enabled);
    reg_confirm_pass_->setEnabled(enabled);
    reg_btn_->setEnabled(enabled);
}

// ── 登录点击 ───────────────────────────────────────────────

void LoginDialog::on_login_clicked() {
    QString email = login_email_->text().trimmed();
    QString pass  = login_pass_->text();

    if (email.isEmpty() || pass.isEmpty()) {
        login_status_->setText("请输入邮箱和密码");
        login_status_->show();
        return;
    }

    // 禁用按钮，显示状态
    set_controls_enabled(false);
    login_btn_->setText("正在验证...");
    login_status_->setText("正在连接服务器...");
    login_status_->setStyleSheet("QLabel { color: #888; font-size: 13px; }");
    login_status_->show();
    waiting_for_response_ = true;

    // 连接到服务器
    sock_->connectToHost(server_host_, server_port_);
    timeout_timer_->start(8000); // 8 秒超时
}

void LoginDialog::send_login_request(const QString& email, const QString& password) {
    wemeet::LoginReq req;
    req.set_email(email.toStdString());
    req.set_password(password.toStdString());

    QByteArray packet = pack_message(
        static_cast<int>(wemeet::MSG_LOGIN_REQ),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
        req);

    sock_->write(packet);
    sock_->flush();
    login_status_->setText("正在验证账号...");
}

// ── 注册点击 ───────────────────────────────────────────────

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

    set_controls_enabled(false);
    reg_btn_->setText("注册中...");
    waiting_for_response_ = true;

    sock_->connectToHost(server_host_, server_port_);
    timeout_timer_->start(8000);
}

void LoginDialog::send_register_request(const QString& email, const QString& password,
                                         const QString& nickname) {
    wemeet::RegisterReq req;
    req.set_email(email.toStdString());
    req.set_password(password.toStdString());
    req.set_nickname(nickname.toStdString());

    QByteArray packet = pack_message(
        static_cast<int>(wemeet::MSG_REGISTER_REQ),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
        req);

    sock_->write(packet);
    sock_->flush();
}

// ── 网络事件 ───────────────────────────────────────────────

void LoginDialog::on_socket_connected() {
    if (!waiting_for_response_) return;

    // 检测是登录还是注册（通过检查正在 disable 的是哪个按钮）
    if (!login_btn_->isEnabled()) {
        send_login_request(login_email_->text().trimmed(), login_pass_->text());
    } else {
        send_register_request(reg_email_->text().trimmed(),
                               reg_pass_->text(),
                               reg_nickname_->text().trimmed());
    }
}

void LoginDialog::on_socket_ready_read() {
    recv_buf_.append(sock_->readAll());

    // 解析粘包：4 字节长度头 + body
    while (recv_buf_.size() >= 4) {
        uint32_t body_len = qFromBigEndian(
            *reinterpret_cast<const uint32_t*>(recv_buf_.constData()));
        if (body_len > 64 * 1024 * 1024) {
            recv_buf_.clear();
            sock_->close();
            return;
        }
        if (recv_buf_.size() < 4 + static_cast<int>(body_len)) break;

        QByteArray body = recv_buf_.mid(4, body_len);
        recv_buf_.remove(0, 4 + body_len);

        // 解析 BaseMessage
        wemeet::BaseMessage base;
        if (!base.ParseFromString(std::string(body.constData(), body.size()))) continue;

        if (!waiting_for_response_) continue;

        timeout_timer_->stop();
        waiting_for_response_ = false;

        if (base.type() == wemeet::MSG_LOGIN_RESP) {
            wemeet::LoginResp resp;
            if (resp.ParseFromString(base.payload()) && resp.success()) {
                // 登录成功
                uint64_t uid = resp.user_id();
                QString nick = QString::fromStdString(resp.nickname());
                sock_->close();

                // 恢复按钮状态后关闭
                set_controls_enabled(true);
                login_btn_->setText("登录");
                emit login_success(uid, nick);
                hide();
                return;
            } else {
                // 登录失败
                set_controls_enabled(true);
                login_btn_->setText("登录");
                login_status_->setText(
                    QString("登录失败: %1")
                        .arg(QString::fromStdString(resp.error_msg().empty()
                            ? "邮箱或密码错误" : resp.error_msg())));
                login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; }");
                login_status_->show();
                sock_->close();
            }

        } else if (base.type() == wemeet::MSG_REGISTER_RESP) {
            wemeet::RegisterResp resp;
            if (resp.ParseFromString(base.payload()) && resp.success()) {
                set_controls_enabled(true);
                reg_btn_->setText("注册");
                sock_->close();

                // 注册成功，自动切换到登录标签页并填充邮箱
                QMessageBox::information(this, "成功", "注册成功！请输入密码登录。");
                auto* tabs = findChild<QTabWidget*>();
                if (tabs) tabs->setCurrentIndex(0);
                login_email_->setText(reg_email_->text());
                login_pass_->setFocus();
            } else {
                set_controls_enabled(true);
                reg_btn_->setText("注册");
                sock_->close();
                login_status_->setText(
                    QString("注册失败: %1")
                        .arg(QString::fromStdString(resp.error_msg().empty()
                            ? "该邮箱已被注册" : resp.error_msg())));
                login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; }");
                login_status_->show();
            }
        }
    }
}

void LoginDialog::on_socket_error(QAbstractSocket::SocketError /*err*/) {
    if (!waiting_for_response_) return;
    waiting_for_response_ = false;
    timeout_timer_->stop();

    set_controls_enabled(true);
    login_btn_->setText("登录");
    reg_btn_->setText("注册");

    login_status_->setText(
        QString("连接失败: %1").arg(sock_->errorString()));
    login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; }");
    login_status_->show();
}

void LoginDialog::on_login_timeout() {
    if (!waiting_for_response_) return;
    waiting_for_response_ = false;
    sock_->close();

    set_controls_enabled(true);
    login_btn_->setText("登录");
    reg_btn_->setText("注册");

    login_status_->setText("连接超时，请检查服务器是否运行");
    login_status_->setStyleSheet("QLabel { color: #E74C3C; font-size: 13px; }");
    login_status_->show();
}

// ── 关闭 / 窗口事件 ────────────────────────────────────────

void LoginDialog::on_close_clicked() {
    reject();
}

bool LoginDialog::eventFilter(QObject* obj, QEvent* event) {
    if (obj == title_bar_ && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            drag_pos_ = me->globalPosition().toPoint() - frameGeometry().topLeft();
            dragging_ = true;
        }
    }
    if (obj == title_bar_ && event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (dragging_ && (me->buttons() & Qt::LeftButton))
            move(me->globalPosition().toPoint() - drag_pos_);
    }
    if (obj == title_bar_ && event->type() == QEvent::MouseButtonRelease)
        dragging_ = false;
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
