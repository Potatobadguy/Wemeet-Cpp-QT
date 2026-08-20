#include "window_enumerator.h"

#include <QGuiApplication>
#include <QScreen>
#include <QImage>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <dwmapi.h>
#endif

// ════════════════════════════════════════════════════════════
//  Windows 实现：Win32 EnumWindows + DWM + PrintWindow
// ════════════════════════════════════════════════════════════
#if defined(Q_OS_WIN)

namespace {

// EnumWindows 回调上下文
struct EnumContext {
    QList<WindowInfo>* result;
};

/**
 * @brief 过滤规则：可见顶层窗口、有标题、非 DWM 遮蔽、非工具窗口
 */
BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lparam);

    // 仅可见窗口
    if (!::IsWindowVisible(hwnd)) return TRUE;

    // 排除被 DWM 遮蔽的窗口（如虚拟桌面外的窗口）
    BOOL cloaked = FALSE;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        if (cloaked) return TRUE;
    }

    // 排除工具窗口（WS_EX_TOOLWINDOW 不出现在任务栏，通常无共享价值）
    LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return TRUE;

    // 必须有非空标题
    wchar_t title[256] = {0};
    ::GetWindowTextW(hwnd, title, 255);
    if (title[0] == L'\0') return TRUE;

    WindowInfo info;
    info.id    = reinterpret_cast<uintptr_t>(hwnd);
    info.title = QString::fromWCharArray(title);

    // 窗口图标：优先 WM_GETICON 大图标，退回类图标
    HICON hicon = reinterpret_cast<HICON>(
        ::SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
    if (!hicon) {
        hicon = reinterpret_cast<HICON>(
            ::SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    }
    if (!hicon) {
        hicon = reinterpret_cast<HICON>(::GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    // QIcon 从 HICON 构造需要 QtWinExtras（Qt6 已移除）；
    // 本项目保持零新依赖，缩略图已足够辨识窗口，图标留空。
    (void)hicon;

    ctx->result->append(info);
    return TRUE;
}

} // namespace

QList<WindowInfo> WindowEnumerator::enumerate_windows() {
    QList<WindowInfo> result;
    EnumContext ctx{&result};
    ::EnumWindows(&enum_windows_proc, reinterpret_cast<LPARAM>(&ctx));

    // 为每个窗口抓缩略图（数量通常 <50，对话框打开时一次性抓取可接受）
    for (auto& info : result) {
        info.thumbnail = capture_thumbnail(info.id, QSize(320, 200));
    }
    return result;
}

bool WindowEnumerator::is_window_valid(uintptr_t id) {
    return ::IsWindow(reinterpret_cast<HWND>(id)) != FALSE;
}

bool WindowEnumerator::is_window_minimized(uintptr_t id) {
    return ::IsIconic(reinterpret_cast<HWND>(id)) != FALSE;
}

QPixmap WindowEnumerator::capture_thumbnail(uintptr_t id, const QSize& max_size) {
    HWND hwnd = reinterpret_cast<HWND>(id);
    if (!::IsWindow(hwnd)) return QPixmap();

    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) return QPixmap();
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return QPixmap();

    // PrintWindow 到内存 DC（PW_RENDERFULLCONTENT 对 DWM 合成窗口更可靠）
    HDC screen_dc = ::GetDC(nullptr);
    HDC mem_dc    = ::CreateCompatibleDC(screen_dc);
    HBITMAP bmp   = ::CreateCompatibleBitmap(screen_dc, w, h);
    HGDIOBJ old   = ::SelectObject(mem_dc, bmp);

    BOOL ok = ::PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT);

    QPixmap result;
    if (ok) {
        // GetDIBits → QImage（BGRA → RGB32）
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;   // 负值 = 自上而下
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        QImage img(w, h, QImage::Format_RGB32);
        if (::GetDIBits(mem_dc, bmp, 0, h, img.bits(), &bmi, DIB_RGB_COLORS)) {
            result = QPixmap::fromImage(
                img.scaled(max_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }

    ::SelectObject(mem_dc, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);
    return result;
}

// ════════════════════════════════════════════════════════════
//  Linux / 其他平台：降级 stub —— 不支持窗口级共享（仅整屏）
// ════════════════════════════════════════════════════════════
#else

QList<WindowInfo> WindowEnumerator::enumerate_windows() {
    // 降级：返回空列表，UI 层提示"当前平台仅支持整屏共享"
    return {};
}

bool WindowEnumerator::is_window_valid(uintptr_t /*id*/) {
    // 非 Windows 平台不会产生窗口源；返回 true 避免误报失效
    return true;
}

bool WindowEnumerator::is_window_minimized(uintptr_t /*id*/) {
    return false;
}

QPixmap WindowEnumerator::capture_thumbnail(uintptr_t /*id*/, const QSize& /*max_size*/) {
    return QPixmap();
}

#endif // Q_OS_WIN
