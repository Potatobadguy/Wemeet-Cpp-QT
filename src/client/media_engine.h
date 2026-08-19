#pragma once
#include <QObject>
#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QVideoFrame>
#include <QVideoSink>
#include <QMediaCaptureSession>
#include <QCamera>
#include <QAudioSource>
#include <QAudioSink>
#include <QBuffer>
#include <QByteArray>
#include <QVideoWidget>
#include <QElapsedTimer>
#include <unordered_map>
#include <QMutex>
#include <QQueue>
#include <QTimer>
#include "rtp_session.h"
#include "v4l2_capture.h"
#include "screen_share_controller.h"   // ScreenShareController / ShareSource / ShareState
#include <memory>
#include <vector>
#include <cstdint>

/**
 * @brief 远端视频渲染控件 — 自定义 QWidget 高效渲染 QVideoFrame
 *
 * 支持：直接 QPainter 绘制、帧率统计、分辨率自适应
 */
class RemoteVideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit RemoteVideoWidget(QWidget* parent = nullptr);
    ~RemoteVideoWidget() = default;

    void present_frame(const QVideoFrame& frame);
    void set_placeholder_text(const QString& text);
    void set_video_on(bool on);

    /**
     * @brief 远端屏幕共享暂停叠加提示（#20 观看端）
     * @param paused true = 叠加半透明「对方已暂停共享」；false = 解除
     */
    void set_share_paused_hint(bool paused);
    bool share_paused_hint() const { return share_paused_hint_; }

    // 统计
    double fps() const { return fps_; }
    QSize  video_resolution() const { return current_size_; }

signals:
    void first_frame_rendered();
    void frame_rate_changed(double fps);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void update_fps();

    QImage    current_frame_;
    QString   placeholder_text_;
    bool      video_on_ = false;
    bool      share_paused_hint_ = false;   // 远端共享暂停叠加提示

    // 帧率统计
    QElapsedTimer fps_timer_;
    int           frame_count_ = 0;
    double        fps_ = 0.0;
    QSize         current_size_;

    QMutex       frame_mutex_;
};

/**
 * @brief 媒体引擎 — 音视频采集/播放与 RTP 传输集成
 *
 * 核心职责：
 * 1. 本地摄像头/麦克风采集
 * 2. 通过 RtpSession 发送音视频
 * 3. 接收远端 RTP 包、解抖动、渲染/播放
 * 4. 网络自适应（带宽估计、FEC）
 * 5. 统计上报
 */
class MediaEngine : public QObject {
    Q_OBJECT
public:
    explicit MediaEngine(QObject* parent = nullptr);
    ~MediaEngine();

    // ── 生命周期 ─────────────────────────────────────────
    bool initialize(uint64_t user_id, const QString& room_id);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // ── 本地媒体 ────────────────────────────────────────
    bool start_camera(const QByteArray& camera_id = QByteArray());
    void stop_camera();
    bool camera_active() const { return camera_active_ || v4l2_active_; }

    bool start_microphone();
    void stop_microphone();
    bool mic_active() const { return mic_active_; }

    /**
     * @brief 开始屏幕共享（重载：任意 ShareSource，#19）
     *
     * 委托 ScreenShareController 抓帧/帧差/自适应，帧经
     * on_share_frame() JPEG 编码（质量参数化）后由 RtpSession 发送。
     */
    bool start_screen_share(const ShareSource& source);
    bool start_screen_share(int screen_index = 0);   // 便捷重载：整屏共享

    /**
     * @brief 暂停/恢复共享（#20）：停抓帧 + 信令广播 MediaControl(SCREEN, mute)
     */
    void pause_screen_share();
    void resume_screen_share();
    void stop_screen_share();
    bool screen_sharing() const { return screen_sharing_; }

    // ── 远端媒体 ─────────────────────────────────────────
    RemoteVideoWidget* create_remote_video_widget(uint64_t user_id, QWidget* parent);
    void remove_remote_video_widget(uint64_t user_id);
    void clear_remote_video_widgets();   // 清空所有远端 widget（切换/重进会议时调用）
    RemoteVideoWidget* get_remote_widget(uint64_t user_id) const;

    // ── 传输 ─────────────────────────────────────────────
    void set_relay_server(const QHostAddress& host, uint16_t video_port, uint16_t audio_port);
    void set_ssrc(uint32_t video_ssrc, uint32_t audio_ssrc);
    uint16_t local_video_port() const;
    uint16_t local_audio_port() const;
    void set_peer_ssrc(uint64_t user_id, uint32_t ssrc);

    // ── 控制 ─────────────────────────────────────────────
    void mute_audio(bool mute);
    void mute_video(bool mute);
    bool audio_muted() const { return audio_muted_; }
    bool video_muted() const { return video_muted_; }

    // 设置本地预览控件
    void set_local_preview(RemoteVideoWidget* widget);
    RemoteVideoWidget* local_preview_widget() const { return local_preview_widget_; }

    // ── 带宽/质量 ───────────────────────────────────────
    uint32_t current_bandwidth() const;
    int32_t  network_quality() const;
    RtpSession::Stats get_stats() const;
    // 应用服务端下发的带宽调整建议（限制上行码率上限）
    void apply_bandwidth_hint(uint32_t max_bitrate_kbps);

    // ── 中继注册 ─────────────────────────────────────────
    void register_media_relay(const QString& relay_host, uint16_t relay_port);

signals:
    void local_video_frame(const QVideoFrame& frame);
    void remote_video_frame(uint64_t user_id, const QVideoFrame& frame);
    void remote_audio_data(const QByteArray& pcm_data);
    void stats_ready(const RtpSession::Stats& stats);
    void bandwidth_changed(uint32_t new_kbps);
    void network_quality_changed(int32_t quality);
    void screen_share_started();
    void screen_share_stopped();
    void screen_share_paused();                        // #20 本地共享已暂停
    void screen_share_resumed();                       // #20 本地共享已恢复
    void share_source_invalidated(const QString& reason);   // #24 共享源失效
    void error_occurred(const QString& error);
    // 通过信令通道发送序列化的 Protobuf 消息
    void signaling_message(const QByteArray& serialized_data);

    // 麦克风音量电平（0.0 ~ 1.0）
    void volume_level_changed(double level);

private slots:
    void on_rtp_packet_received(const RTPPacket& packet, const QHostAddress& src, uint16_t port);
    // 处理单个 RTP 媒体包（先入 JitterBuffer 统计丢包，再按原始顺序交给 FrameAssembler）
    void process_rtp_packet(const RTPPacket& packet);
    void on_bandwidth_estimated(uint32_t new_bitrate);
    void on_quality_changed(int32_t quality);
    void on_video_frame_captured(const QVideoFrame& frame);
    void on_v4l2_frame_captured(const QVideoFrame& frame);
    // 屏幕共享帧回调：JPEG 编码（quality 来自当前自适应档位）+ RTP 发送
    void on_share_frame(const QImage& frame, int jpeg_quality);
    void on_share_state_changed(ShareState state);

private:
    void start_capture_timer();
    void stop_capture_timer();
    bool try_v4l2_capture();

    // ── 状态 ─────────────────────────────────────────────
    bool initialized_ = false;
    uint64_t user_id_ = 0;
    QString  room_id_;

    // ── 本地摄像头（Qt Multimedia） ──────────────────────
    std::unique_ptr<QCamera> camera_;
    std::unique_ptr<QMediaCaptureSession> capture_session_;
    std::unique_ptr<QVideoSink> video_sink_;
    RemoteVideoWidget* local_preview_widget_ = nullptr;  // 本地预览控件（不拥有）
    bool camera_active_ = false;
    bool camera_available_ = false;

    // ── V4L2 摄像头（Qt 不可用时的降级方案） ─────────────
    std::unique_ptr<V4L2Capture> v4l2_capture_;
    bool v4l2_active_ = false;

    // ── 本地麦克风 ───────────────────────────────────────
    std::unique_ptr<QAudioSource> audio_source_;
    std::unique_ptr<QIODevice> audio_io_;
    bool mic_active_ = false;
    bool audio_muted_ = false;
    bool video_muted_ = false;

    // ── 屏幕共享（委托 ScreenShareController，#20-24）─────
    bool screen_sharing_ = false;
    ScreenShareController* share_controller_ = nullptr;   // Qt 父子机制管理
    void send_screen_control(bool mute);   // MediaControl{SCREEN, mute} 冻结标志

    // ── RTP 传输 ────────────────────────────────────────
    std::unique_ptr<RtpSession> rtp_session_;
    std::unique_ptr<JitterBuffer> jitter_buffer_;
    std::unique_ptr<BandwidthEstimator> bandwidth_estimator_;

    QHostAddress relay_host_;
    uint16_t     relay_video_port_ = 0;
    uint16_t     relay_audio_port_ = 0;
    uint32_t     video_ssrc_ = 0;
    uint32_t     audio_ssrc_ = 0;

    // ── 远端视频控件 ─────────────────────────────────────
    // 注意：widget 由 Qt 父-子对象系统独占管理（gallery_container_），
    // 这里只持裸指针，避免 unique_ptr 与 Qt 父子同时释放导致 double-free
    struct RemoteStream {
        uint64_t user_id;
        RemoteVideoWidget* widget = nullptr;
        uint32_t ssrc;
    };
    std::vector<RemoteStream> remote_streams_;

    // 等待 widget 创建的 peer SSRC 映射
    std::unordered_map<uint64_t, uint32_t> pending_peer_ssrc_;

    // ── 帧重组缓冲（按 SSRC 缓存 JPEG 帧分片）────────────
    struct FrameAssembler {
        uint32_t        ssrc      = 0;
        uint32_t        timestamp = 0;
        QByteArray      jpeg_data;
        uint16_t        expected_next_seq = 0;
        bool            active    = false;
        QElapsedTimer   last_update;
        uint32_t        missing_packets   = 0;   // 未能还原的缺失分片数
        uint32_t        lost_compensated   = 0;  // 由 FEC 还原的分片数
    };
    std::unordered_map<uint32_t, FrameAssembler> frame_assemblers_;

    // ── 统计定时器 ───────────────────────────────────────
    QTimer* stats_timer_ = nullptr;

    // ── 采集线程 ─────────────────────────────────────────
    QTimer* capture_timer_ = nullptr;

    // ── 音量监测 ─────────────────────────────────────────
    QTimer* volume_monitor_timer_ = nullptr;

    // ── 帧率控制 ─────────────────────────────────────────
    static constexpr int kVideoFps = 30;
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioChannels = 1;
    static constexpr int kAudioSampleSize = 16;
};
