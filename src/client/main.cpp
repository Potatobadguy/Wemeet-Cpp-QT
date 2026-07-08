/**
 * @brief WeMeet Qt 客户端 — 入口
 */
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include "main_window.h"

// 内嵌暗色主题样式
static const char* kDarkTheme = R"(
QMainWindow, QDialog, QWidget { background-color: #0a0a1a; color: #e0e0e0; }
QPushButton { background-color: #2a2a3e; color: #e0e0e0; border: none; border-radius: 6px; padding: 8px 16px; }
QPushButton:hover { background-color: #3a3a5e; }
QLineEdit, QTextEdit { background-color: #16213e; color: #e0e0e0; border: 1px solid #2a2a4e; border-radius: 6px; padding: 8px; }
QLineEdit:focus { border-color: #4A90D9; }
QListWidget { background-color: #111122; border: 1px solid #2a2a3e; border-radius: 8px; }
QListWidget::item { padding: 12px; border-radius: 6px; color: #ccc; }
QListWidget::item:hover { background-color: #1a1a3e; }
QListWidget::item:selected { background-color: #4A90D9; color: white; }
QScrollBar:vertical { background: #0a0a1a; width: 8px; }
QScrollBar::handle:vertical { background: #3a3a5e; border-radius: 4px; min-height: 30px; }
QTabWidget::pane { border: 1px solid #2a2a3e; background: #0a0a1a; }
QStatusBar { background-color: #1a1a2e; color: #888; }
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
    app.setStyleSheet(kDarkTheme);

    MainWindow window;
    window.setWindowTitle("WeMeet — 企业级视频会议");
    window.resize(1200, 800);
    window.show();

    return app.exec();
}
