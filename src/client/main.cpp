/**
 * @brief WeMeet Qt 客户端 — 入口
 */
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QObject>
#include "main_window.h"
#include "login_dialog.h"

// 内嵌明亮主题样式（现代圆角、柔和配色）
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

// 设置一个支持中文的字体，按平台优先级尝试
static void setupCjkFont() {
    const char* preferredFonts[] = {
        "Microsoft YaHei",       // Windows 最常用
        "Microsoft YaHei UI",
        "SimHei",
        "Noto Sans CJK SC",      // Linux 常见
        "Noto Sans Mono CJK SC",
        "WenQuanYi Micro Hei",
        "PingFang SC",           // macOS
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

    // 如果没有找到中文字体，使用系统默认字体但设置大字号以触发字体回退
    QFont font = QApplication::font();
    font.setPointSize(10);
    QApplication::setFont(font);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("WeMeet");
    app.setApplicationVersion("1.0.0");

    setupCjkFont();
    app.setStyleSheet(kLightTheme);

    // 先创建主窗口（初始隐藏）
    MainWindow main_window;
    main_window.setWindowTitle("WeMeet — 企业级视频会议");
    main_window.resize(1200, 800);

    // 显示登录对话框（独立窗口）
    LoginDialog login_dialog;
    login_dialog.setWindowTitle("WeMeet — 登录");

    QObject::connect(&login_dialog, &LoginDialog::login_success,
                     [&main_window]() {
        main_window.show();
        main_window.raise();
        main_window.activateWindow();
    });

    login_dialog.exec();

    return app.exec();
}
