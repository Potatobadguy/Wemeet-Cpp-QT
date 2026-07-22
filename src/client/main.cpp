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
#include "main_window.h"
#include "login_dialog.h"

// 内嵌明亮主题样式
static const char* kLightTheme = R"(
QMainWindow, QDialog, QWidget { background-color: #f0f4f8; color: #333333; }
QPushButton { background-color: #4A90D9; color: white; border: none; border-radius: 8px; padding: 10px 20px; font-weight: 500; }
QPushButton:hover { background-color: #357ABD; }
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
    app.setApplicationName("WeMeet");
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
        // 主窗口负责向服务器发送 LOGIN_REQ 并接收响应
        main_window.login_via_server(email, password);
        // 主窗口认证成功后会自动回调 on_login_success 并关闭此对话框
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
                     [&login_dialog](const QString& error_msg) {
        login_dialog.show_error(error_msg);
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

    if (login_dialog.exec() == QDialog::Accepted) {
        return app.exec();
    }
    return 0;
}
