#pragma once
/**
 * @file window_enumerator.h
 * @brief 窗口枚举平台抽象 + 共享源描述结构
 *
 * 平台策略（#19）：
 *   - Windows：Win32 EnumWindows API 枚举可见顶层窗口，
 *     DwmGetWindowAttribute 排除被遮蔽/虚拟窗口，PrintWindow 抓缩略图；
 *   - Linux/其他：降级 stub —— enumerate_windows() 返回空列表，
 *     UI 层据此提示"仅支持整屏共享"。
 *
 * ShareSource 为平台中立结构，定义于此（而非对话框/控制器头），
 * 供 ShareSourceDialog（产出）、ScreenShareController（消费）、
 * MediaEngine（转发）三方共享，避免 UI ↔ 控制层循环依赖。
 */

#include <QString>
#include <QIcon>
#include <QPixmap>
#include <QList>
#include <cstdint>

/**
 * @brief 可枚举的应用窗口信息
 */
struct WindowInfo {
    uintptr_t id = 0;        // 窗口句柄（Windows: HWND 值）
    QString   title;         // 窗口标题
    QIcon     icon;          // 窗口图标（可为空）
    QPixmap   thumbnail;     // 窗口缩略图（可为空）
};

/**
 * @brief 共享源描述 — ShareSourceDialog 产出，ScreenShareController 消费
 */
struct ShareSource {
    enum class Type : uint8_t {
        SCREEN = 0,   // 整屏共享（screen_index 指定第几块屏）
        WINDOW = 1    // 窗口级共享（window_id 指定 HWND，仅 Windows）
    };

    Type      type = Type::SCREEN;
    int       screen_index = 0;   // Type::SCREEN 时有效
    uintptr_t window_id = 0;      // Type::WINDOW 时有效（HWND）
    QString   title;              // 展示用标题（屏幕名/窗口名）

    bool is_window() const { return type == Type::WINDOW; }
};

/**
 * @brief 窗口枚举器（平台抽象，全静态接口）
 */
class WindowEnumerator {
public:
    /**
     * @brief 枚举当前所有可见顶层窗口
     * @return Windows: 可见且非最小化要求的窗口列表（含缩略图）；
     *         Linux/其他: 空列表（调用方降级为仅整屏共享）
     */
    static QList<WindowInfo> enumerate_windows();

    /**
     * @brief 窗口是否仍然有效（存在且未被销毁）
     *        共享过程中用于"源失效检测"（#24）
     */
    static bool is_window_valid(uintptr_t id);

    /**
     * @brief 窗口是否处于最小化状态（最小化时抓帧无意义，暂停降速）
     */
    static bool is_window_minimized(uintptr_t id);

    /**
     * @brief 抓取窗口缩略图（供对话框展示）
     * @param max_size 缩略图最大尺寸（保持纵横比缩放）
     */
    static QPixmap capture_thumbnail(uintptr_t id, const QSize& max_size);
};
