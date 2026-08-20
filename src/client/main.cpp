/**
 * @brief WeMeet Qt 客户端 — 入口
 */
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QObject>
#include <QMessageBox>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include "main_window.h"
#include "login_dialog.h"

// ── 调试便利：自动登录凭据（QSettings 持久化）────────────
// 仅本地调试用，明文保存便于跳过登录流程；生产环境请改用 token。
static const QString kSettingsAutoLogin = "AutoLogin";
static const QString kSettingsEmail     = "AutoLogin/email";
static const QString kSettingsPassword  = "AutoLogin/password";
static const QString kSettingsRemember  = "AutoLogin/remember_password";

// 内嵌明亮主题样式
static const char* kLightTheme = R"(
QMainWindow, QDialog, QWidget { background-color: #f0f4f8; color: #333333; }
QPushButton { background-color: #4A90D9; color: white; border: none; border-radius: 8px; padding: 10px 20px; min-height: 36px; font-weight: 500; }
QPushButton:hover { background-color: #357ABD; }
QPushButton:pressed { background-color: #2A5F9E; }
QLineEdit, QTextEdit { background-color: #ffffff; color: #333333; border: 1px solid #d0d0d0; border-radius: 8px; padding: 8px; }
QLineEdit:focus { border-color: #4A90D9; }
QListWidget { background-color: #ffffff; border: 1px solid #d0d0d0; border-radius: 8px; }
QListWidget::item { padding: 12px; border-radius: 6px; color: #333333; }
QListWidget::item:hover { background-color: #f0f0f0; }
QListWidget::item:selected { background-color: #4A90D9; color: white; }
QScrollBar:vertical { background: #f0f0f0; width: 8px; border-radius: 4px; }
QScrollBar::handle:vertical { background: #c0c0c0; border-radius: 4px; min-height: 30px; }
QTabWidget::pane { border: none; background: transparent; }
QStatusBar { background-color: #ffffff; color: #666666; }
)";

static void setupCjkFont() {
    const char* preferredFonts[] = {
        "Microsoft YaHei",
        "Microsoft YaHei UI",
        "SimHei",
        "Noto Sans CJK SC",
        "Noto Sans Mono CJK SC",
        "WenQuanYi Micro Hei",
        "PingFang SC",
        "Heiti SC"
    };
    for (const char* name : preferredFonts) {
        if (QFontDatabase::hasFamily(name)) {
            QFont font(name);
            font.setPointSize(10);
            QApplication::setFont(font);
            return;
        }
    }
    QFont font = QApplication::font();
    font.setPointSize(10);
    QApplication::setFont(font);
}

// 将窗口居中到主屏幕的可用区域（WSLg/Wayland 下默认位置常导致窗口跑出可见区）
static void centerOnPrimaryScreen(QWidget* w) {
    if (!w) return;
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    QRect avail = screen->availableGeometry();
    QSize  sz    = w->size();
    int x = avail.x() + (avail.width()  - sz.width())  / 2;
    int y = avail.y() + (avail.height() - sz.height()) / 2;
    // 防止多屏幕下出现负坐标把窗口推出可见区
    if (x < avail.x()) x = avail.x();
    if (y < avail.y()) y = avail.y();
    w->move(x, y);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("WeMeet");  // QSettings 定位持久化文件需要
    QCoreApplication::setApplicationName("WeMeet");
    app.setApplicationVersion("1.0.0");

    setupCjkFont();
    app.setStyleSheet(kLightTheme);

    // 创建主窗口（初始隐藏）
    MainWindow main_window;
    main_window.setWindowTitle("WeMeet — 企业级视频会议");
    main_window.resize(1200, 800);
    centerOnPrimaryScreen(&main_window);

    // 登录对话框（只采集凭据，网络认证由 MainWindow 通过 NetworkClient 完成）
    LoginDialog login_dialog;
    login_dialog.setWindowTitle("WeMeet — 登录");
    centerOnPrimaryScreen(&login_dialog);

    QObject::connect(&login_dialog, &LoginDialog::login_request,
                     [&main_window, &login_dialog](const QString& email, const QString& password) {
        // 凭据持久化（调试便利）：始终保留 email；仅在勾选"记住密码"时保存密码
        QSettings settings;
        settings.setValue(kSettingsEmail, email);
        settings.setValue(kSettingsRemember, login_dialog.login_remember_pass_
                                                  ? login_dialog.login_remember_pass_->isChecked()
                                                  : false);
        if (login_dialog.login_remember_pass_ && login_dialog.login_remember_pass_->isChecked()) {
            settings.setValue(kSettingsPassword, password);
        } else {
            settings.remove(kSettingsPassword);
        }
        // 主窗口负责向服务器发送 LOGIN_REQ 并接收响应
        main_window.login_via_server(email, password);
    });

    QObject::connect(&login_dialog, &LoginDialog::register_request,
                     [&main_window](const QString& email, const QString& password, const QString& nickname) {
        // 主窗口负责向服务器发送 REGISTER_REQ 并接收响应
        main_window.register_via_server(email, password, nickname);
    });

    // 主窗口认证成功的信号 -> 关闭登录对话框
    QObject::connect(&main_window, &MainWindow::auth_success,
                     [&main_window, &login_dialog](uint64_t user_id, const QString& nickname) {
        login_dialog.hide();
        main_window.on_login_success(user_id, nickname);
        // 显式 showNormal：避免 WSLg/Wayland 下窗口以最小化或不可见状态显示
        if (main_window.isMinimized()) main_window.showNormal();
        main_window.showNormal();
        main_window.raise();
        main_window.activateWindow();
        // 重新居中（on_login_success 内部可能改了尺寸）
        centerOnPrimaryScreen(&main_window);
        login_dialog.accept();
    });

    // 主窗口认证失败的信号 -> 显示错误
    QObject::connect(&main_window, &MainWindow::auth_failed,
                     [&main_window, &login_dialog](const QString& error_msg) {
        login_dialog.show_error(error_msg);
        // 自动登录失败（凭据失效/服务器重启）→ 重新显示登录对话框让用户手动登录
        if (!login_dialog.isVisible()) {
            centerOnPrimaryScreen(&login_dialog);
            login_dialog.show();
            login_dialog.raise();
            login_dialog.activateWindow();
        }
    });

    // 主窗口注册响应的信号 -> 显示消息
    QObject::connect(&main_window, &MainWindow::register_result,
                     [&login_dialog](bool success, const QString& msg) {
        login_dialog.show_register_result(success, msg);
    });

    // 先连接服务器
    if (!main_window.connect_to_server()) {
        QMessageBox::critical(nullptr, "连接失败",
            "无法连接到信令服务器，请确认服务端已启动。\n\n"
            "服务器地址: " + main_window.server_host() + ":" +
            QString::number(main_window.server_port()));
        return 1;
    }

    // ── 调试便利：读出上次保存的账号/密码填到登录界面（不自动登录） ──
    // 勾选"记住密码"只意味着下次仍弹登录界面但凭据已填好，需用户手动点登录
    QSettings settings;
    const QString saved_email = settings.value(kSettingsEmail).toString();
    const QString saved_pass  = settings.value(kSettingsPassword).toString();
    const bool    saved_remember = settings.value(kSettingsRemember, false).toBool();
    if (!saved_email.isEmpty()) {
        login_dialog.prefill_credentials(saved_email, saved_pass, saved_remember);
        qDebug("Prefilled saved credentials for %s (remember=%d)",
               qPrintable(saved_email), saved_remember ? 1 : 0);
    }

    // 始终弹登录界面，由用户手动点登录（即便勾选了"记住密码"也不自动登录）
    if (login_dialog.exec() == QDialog::Accepted) {
        return app.exec();
    }
    return 0;
}
