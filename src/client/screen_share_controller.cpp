#include "screen_share_controller.h"

#include <QGuiApplication>
#include <QScreen>
#include <QPixmap>
#include <QDebug>
#include <cstring>

ScreenShareController::ScreenShareController(QObject* parent)
    : QObject(parent) {
    capture_timer_.setSingleShot(false);
    connect(&capture_timer_, &QTimer::timeout,
            this, &ScreenShareController::on_capture_tick);
}

// ── 生命周期 ────────────────────────────────────────────────

bool ScreenShareController::start(const ShareSource& source) {
    // 支持"源失效后重新选择"（#24）：非 Idle 状态先软复位再按新源启动
    if (state_ != ShareState::Idle) {
        capture_timer_.stop();
    }

    // 校验源有效性
    if (source.is_window()) {
        if (!WindowEnumerator::is_window_valid(source.window_id)) {
            emit source_invalidated(QStringLiteral("所选窗口不存在或已关闭"));
            return false;
        }
    } else {
        const auto screens = QGuiApplication::screens();
        if (source.screen_index < 0 || source.screen_index >= screens.size()) {
            emit source_invalidated(QStringLiteral("所选屏幕不存在"));
            return false;
        }
    }

    source_ = source;
    prev_frame_ = QImage();
    static_frames_ = 0;
    static_mode_ = false;
    current_tier_ = Tier::Mid;
    high_loss_count_ = 0;
    low_loss_count_ = 0;

    state_ = ShareState::Sharing;
    capture_timer_.start(normal_interval_ms());
    emit state_changed(state_);

    qDebug("ScreenShareController: started, type=%s, title=%s",
           source_.is_window() ? "window" : "screen",
           qPrintable(source_.title));
    return true;
}

void ScreenShareController::stop() {
    if (state_ == ShareState::Idle) return;
    capture_timer_.stop();
    prev_frame_ = QImage();
    static_frames_ = 0;
    static_mode_ = false;
    state_ = ShareState::Idle;
    emit state_changed(state_);
    qDebug("ScreenShareController: stopped");
}

// ── 暂停/恢复（#20）─────────────────────────────────────────

void ScreenShareController::pause() {
    if (state_ != ShareState::Sharing) return;
    capture_timer_.stop();        // 停止抓帧（MediaEngine 负责发 MediaControl 冻结标志）
    state_ = ShareState::Paused;
    emit state_changed(state_);
    qDebug("ScreenShareController: paused");
}

void ScreenShareController::resume() {
    if (state_ != ShareState::Paused) return;
    state_ = ShareState::Sharing;
    // 恢复时强制发送一帧：清空基准帧，下一 tick 必判为"有变化"
    prev_frame_ = QImage();
    static_frames_ = 0;
    static_mode_ = false;
    capture_timer_.start(normal_interval_ms());
    on_capture_tick();            // 立即抓一帧，远端尽快解冻
    emit state_changed(state_);
    qDebug("ScreenShareController: resumed");
}

// ── 抓帧主循环 ──────────────────────────────────────────────

void ScreenShareController::on_capture_tick() {
    if (state_ != ShareState::Sharing) return;

    // #24：源失效检测（窗口被关闭 / 屏幕被断开）
    if (!check_source_alive()) {
        capture_timer_.stop();
        QString reason = source_.is_window()
            ? QStringLiteral("共享窗口「%1」已关闭").arg(source_.title)
            : QStringLiteral("共享屏幕已断开");
        state_ = ShareState::Paused;   // 进入暂停态，等待用户重新选择源
        emit state_changed(state_);
        emit source_invalidated(reason);
        return;
    }

    QImage frame = capture_frame();
    if (frame.isNull()) {
        // capture_frame 内部已打印警告；这里不再重复，避免刷屏
        static std::atomic<uint64_t> null_skip{0};
        uint64_t s = null_skip.fetch_add(1);
        if (s % 25 == 0) {
            qDebug("ScreenShareController: on_capture_tick skipped %llu ticks due to empty frame",
                   static_cast<unsigned long long>(s + 1));
        }
        return;
    }

    // #22：帧差检测 —— 整帧 memcmp
    if (frames_identical(frame, prev_frame_)) {
        ++static_frames_;
        if (static_frames_ >= kStaticThreshold && !static_mode_) {
            // 连续静止 ≥3 帧 → 降速至 1fps 心跳（维持远端画面与统计存活）
            static_mode_ = true;
            capture_timer_.setInterval(kStaticIntervalMs);
        }
        // 静止模式下的 1fps 心跳：仍强制重发当前帧（关键帧），
        // 让远端在首帧/中途丢包后能够自愈，避免永久黑屏。
        if (static_mode_ && static_frames_ % kHeartbeatSendEvery == 0) {
            prev_frame_ = frame;
            emit frame_ready(frame, tier_params(current_tier_).jpeg_quality);
        }
        return;   // 画面无变化，不按正常帧率发送
    }

    // 画面有变化 → 恢复正常帧率并发送
    if (static_mode_) {
        static_mode_ = false;
        capture_timer_.setInterval(normal_interval_ms());
    }
    static_frames_ = 0;
    prev_frame_ = frame;

    emit frame_ready(frame, tier_params(current_tier_).jpeg_quality);
}

QImage ScreenShareController::capture_frame() {
    QPixmap pixmap;

    if (source_.is_window()) {
        // 窗口级抓取（Windows：grabWindow(WId) 经 DWM 重定向）
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) return QImage();
        pixmap = screen->grabWindow(static_cast<WId>(source_.window_id));
        if (pixmap.isNull()) {
            // WSLg/部分合成器上 grabWindow(WId) 也失败，尝试回退到整屏
            pixmap = screen->grabWindow(0);
        }
    } else {
        // 整屏抓取：按 geometry 限定到选中屏（多屏正确性）
        const auto screens = QGuiApplication::screens();
        if (source_.screen_index < 0 ||
            source_.screen_index >= screens.size()) return QImage();
        QScreen* screen = screens[source_.screen_index];
        const QRect geom = screen->geometry();

        // ── 多策略抓取：WSLg 下 grabWindow(0, x, y, w, h) 常返回空 ──
        // 策略 1：按 geometry 限定（首选，多屏正确）
        pixmap = screen->grabWindow(0, geom.x(), geom.y(),
                                    geom.width(), geom.height());
        // 策略 2：不带几何的全屏抓取
        if (pixmap.isNull() || pixmap.width() == 0 || pixmap.height() == 0) {
            pixmap = screen->grabWindow(0);
        }
        // 策略 3：主屏兜底
        if (pixmap.isNull() || pixmap.width() == 0 || pixmap.height() == 0) {
            if (auto* primary = QGuiApplication::primaryScreen()) {
                pixmap = primary->grabWindow(0);
            }
        }
        // 抓到全桌面但只想选中屏 → 按 geom 裁剪
        if (!pixmap.isNull() &&
            (pixmap.width() > geom.width() || pixmap.height() > geom.height())) {
            if (geom.x() + geom.width()  <= pixmap.width() &&
                geom.y() + geom.height() <= pixmap.height()) {
                pixmap = pixmap.copy(geom);
            }
        }
    }

    if (pixmap.isNull() || pixmap.width() == 0 || pixmap.height() == 0) {
        // 抓取彻底失败（如 WSLg 平台限制）：每 25 次打一条警告
        static std::atomic<uint64_t> empty_count{0};
        uint64_t e = empty_count.fetch_add(1);
        if (e == 0 || e % 25 == 0) {
            qWarning("ScreenShareController: capture_frame returned empty pixmap #%llu "
                     "(Wayland/WSLg portal may be needed)",
                     static_cast<unsigned long long>(e + 1));
        }
        return QImage();
    }
    return pixmap.toImage().convertToFormat(QImage::Format_RGB32);
}

/**
 * @brief 整帧 memcmp 帧差检测
 *
 * RGB32 格式下逐行比较（bytesPerLine 可能含行对齐填充，按行处理），
 * 1080p 约 8MB，内存带宽下耗时 <2ms，满足"简单可用"。
 */
bool ScreenShareController::frames_identical(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull()) return false;
    if (a.size() != b.size() || a.format() != b.format()) return false;
    if (a.bytesPerLine() != b.bytesPerLine()) return false;

    const int lines = a.height();
    const qsizetype stride = a.bytesPerLine();
    for (int y = 0; y < lines; ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(stride)) != 0) {
            return false;
        }
    }
    return true;
}

// ── 自适应三档（#21）────────────────────────────────────────

ScreenShareController::TierParams
ScreenShareController::tier_params(Tier t) {
    switch (t) {
    case Tier::High: return {15, 80};
    case Tier::Mid:  return {10, 60};
    case Tier::Low:  return {5,  40};
    }
    return {10, 60};
}

int ScreenShareController::current_jpeg_quality() const {
    return tier_params(current_tier_).jpeg_quality;
}

int ScreenShareController::normal_interval_ms() const {
    return 1000 / tier_params(current_tier_).fps;
}

void ScreenShareController::apply_tier(Tier t) {
    if (current_tier_ == t) return;
    current_tier_ = t;
    // 非静止模式下立即应用新帧率
    if (!static_mode_ && state_ == ShareState::Sharing) {
        capture_timer_.setInterval(normal_interval_ms());
    }
    qDebug("ScreenShareController: tier -> %s (fps=%d, q=%d)",
           t == Tier::High ? "High" : t == Tier::Mid ? "Mid" : "Low",
           tier_params(t).fps, tier_params(t).jpeg_quality);
}

void ScreenShareController::update_network_stats(double loss_rate) {
    if (state_ == ShareState::Idle) return;

    // 迟滞防抖：连续达标才换档，避免单次抖动引发振荡
    if (loss_rate > 0.05) {
        ++high_loss_count_;
        low_loss_count_ = 0;
    } else if (loss_rate < 0.01) {
        ++low_loss_count_;
        high_loss_count_ = 0;
    } else {
        high_loss_count_ = 0;
        low_loss_count_ = 0;
    }

    // 降档：loss>5% 连续 2 次（快速响应拥塞）
    if (high_loss_count_ >= 2) {
        if (current_tier_ == Tier::High)      apply_tier(Tier::Mid);
        else if (current_tier_ == Tier::Mid)  apply_tier(Tier::Low);
        high_loss_count_ = 0;
    }
    // 升档：loss<1% 连续 4 次（缓慢恢复，防止反复横跳）
    else if (low_loss_count_ >= 4) {
        if (current_tier_ == Tier::Low)       apply_tier(Tier::Mid);
        else if (current_tier_ == Tier::Mid)  apply_tier(Tier::High);
        low_loss_count_ = 0;
    }
}

// ── 源存活检查（#24）────────────────────────────────────────

bool ScreenShareController::check_source_alive() {
    if (source_.is_window()) {
        // 窗口被销毁 → 失效；最小化不算失效（IsIconic 时抓帧仍有效或黑帧）
        return WindowEnumerator::is_window_valid(source_.window_id);
    }
    // 屏幕：索引仍在在线屏幕列表内即有效
    const auto screens = QGuiApplication::screens();
    return source_.screen_index >= 0 && source_.screen_index < screens.size();
}
