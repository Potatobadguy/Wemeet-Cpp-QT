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
    bool camera_active() const { return camera_active_; }

    bool start_microphone();
    void stop_microphone();
    bool mic_active() const { return mic_active_; }

    bool start_screen_share(int screen_index = 0);
    void stop_screen_share();
    bool screen_sharing() const { return screen_sharing_; }

    // ── 远端媒体 ─────────────────────────────────────────
    RemoteVideoWidget* create_remote_video_widget(uint64_t user_id, QWidget* parent);
    void remove_remote_video_widget(uint64_t user_id);
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
    void set_local_preview(QVideoWidget* widget);
    QVideoWidget* local_preview_widget() const { return local_preview_widget_; }

    // ── 带宽/质量 ───────────────────────────────────────
    uint32_t current_bandwidth() const;
    int32_t  network_quality() const;
    RtpSession::Stats get_stats() const;

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
    void error_occurred(const QString& error);
    // 通过信令通道发送序列化的 Protobuf 消息
    void signaling_message(const QByteArray& serialized_data);

    // 麦克风音量电平（0.0 ~ 1.0）
    void volume_level_changed(double level);

private slots:
    void on_rtp_packet_received(const RTPPacket& packet, const QHostAddress& src, uint16_t port);
    void on_bandwidth_estimated(uint32_t new_bitrate);
    void on_quality_changed(int32_t quality);
    void on_video_frame_captured(const QVideoFrame& frame);
    void on_v4l2_frame_captured(const QVideoFrame& frame);
    void capture_screen_frame();

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
    QVideoWidget* local_preview_widget_ = nullptr;  // 本地预览控件（不拥有）
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

    // ── 屏幕共享 ─────────────────────────────────────────
    bool screen_sharing_ = false;
    int  screen_index_ = 0;
    QTimer* screen_capture_timer_ = nullptr;

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
    struct RemoteStream {
        uint64_t user_id;
        std::unique_ptr<RemoteVideoWidget> widget;
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
