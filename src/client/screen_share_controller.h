#pragma once
/**
 * @file screen_share_controller.h
 * @brief 屏幕共享控制核心（#20-24）
 *
 * 职责：
 *   - 抓帧：QScreen::grabWindow（整屏按 geometry，窗口按 WId）；
 *   - 帧差检测：RGB32 整帧 memcmp，连续 ≥3 帧静止降速至 1fps 心跳（#22）；
 *   - 自适应三档：High(15fps,q80)/Mid(10fps,q60)/Low(5fps,q40)，
 *     基于本地 RtpSession 丢包率，带迟滞防抖（#21）；
 *   - 暂停/恢复：停止抓帧，由 MediaEngine 发 MediaControl(SCREEN, mute)（#20）；
 *   - 源失效检测：被共享窗口关闭/最小化时触发 source_invalidated（#24）。
 *
 * 本类不做网络发送：抓到的帧经 frame_ready(QImage, jpeg_quality)
 * 信号交给 MediaEngine 编码与发送，保持"控制 / 传输"分离。
 */

#include <QObject>
#include <QImage>
#include <QTimer>
#include "window_enumerator.h"   // ShareSource

/**
 * @brief 共享状态机
 */
enum class ShareState : uint8_t {
    Idle    = 0,   // 未共享
    Sharing = 1,   // 共享中（正常抓帧）
    Paused  = 2    // 已暂停（停止抓帧，远端显示冻结提示）
};

class ScreenShareController : public QObject {
    Q_OBJECT
public:
    explicit ScreenShareController(QObject* parent = nullptr);
    ~ScreenShareController() override = default;

    // ── 生命周期 ─────────────────────────────────────────
    bool start(const ShareSource& source);
    void stop();

    // ── 暂停/恢复（#20）─────────────────────────────────
    void pause();
    void resume();

    ShareState state() const { return state_; }
    const ShareSource& source() const { return source_; }

    // ── 自适应（#21）─────────────────────────────────────
    /**
     * @brief 喂入本地发送侧丢包率（0.0~1.0），驱动档位调整。
     *        由 MediaEngine 统计定时器周期调用（约 1~2s 一次）。
     *        迟滞规则：loss>5% 连续 2 次降档；loss<1% 连续 4 次升档。
     */
    void update_network_stats(double loss_rate);

    /**
     * @brief 当前档位的 JPEG 编码质量（供 MediaEngine 参数化编码）
     */
    int current_jpeg_quality() const;

signals:
    /**
     * @brief 抓到一帧有变化的画面
     * @param frame RGB32 图像
     * @param jpeg_quality 当前档位对应的 JPEG 质量（0-100）
     */
    void frame_ready(const QImage& frame, int jpeg_quality);

    void state_changed(ShareState state);

    /**
     * @brief 共享源失效（窗口被关闭/屏幕断开等，#24）
     * @param reason 人类可读原因
     */
    void source_invalidated(const QString& reason);

private slots:
    void on_capture_tick();

private:
    // ── 自适应档位（#21）─────────────────────────────────
    enum class Tier : uint8_t { High = 0, Mid = 1, Low = 2 };

    struct TierParams {
        int fps;
        int jpeg_quality;
    };
    static TierParams tier_params(Tier t);
    void apply_tier(Tier t);

    // ── 抓帧与帧差（#22）─────────────────────────────────
    QImage capture_frame();                        // 按 source_ 抓一帧（RGB32）
    static bool frames_identical(const QImage& a, const QImage& b);  // 整帧 memcmp
    int  normal_interval_ms() const;               // 当前档位帧率间隔

    // ── 源存活检查（#24）─────────────────────────────────
    bool check_source_alive();

    // ── 状态 ─────────────────────────────────────────────
    ShareState  state_ = ShareState::Idle;
    ShareSource source_;
    QTimer      capture_timer_;

    QImage      prev_frame_;          // 上一帧（帧差比较基准）
    int         static_frames_ = 0;   // 连续静止帧计数
    bool        static_mode_ = false; // 是否已降速到 1fps 心跳

    Tier        current_tier_ = Tier::Mid;   // 默认中档
    int         high_loss_count_ = 0;        // 连续高丢包次数
    int         low_loss_count_  = 0;        // 连续低丢包次数

    // 静止降帧阈值
    static constexpr int kStaticThreshold = 3;      // 连续 ≥3 帧静止 → 1fps
    static constexpr int kStaticIntervalMs = 1000;  // 心跳帧间隔
    static constexpr int kHeartbeatSendEvery = 5;   // 静止心跳每 5 次强制重发一帧（约 5s），供远端自愈
};
