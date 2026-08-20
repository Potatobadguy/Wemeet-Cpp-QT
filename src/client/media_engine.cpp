#include "media_engine.h"
#include "rtp_session.h"
#include "common.pb.h"
#include "media.pb.h"
#include <QVideoFrame>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QAudioDevice>
#include <QAudioSource>
#include <QAudioSink>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QElapsedTimer>
#include <cmath>
#include <chrono>

namespace {

// 辅助：把子消息序列化为 BaseMessage（不含长度头，长度头由 NetworkClient 添加）
QByteArray make_signaling_msg(int msg_type, const std::string& payload_bytes) {
    wemeet::BaseMessage base;
    base.set_type(static_cast<wemeet::MsgType>(msg_type));
    base.set_sequence_id(0);
    base.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    base.set_payload(payload_bytes);
    std::string out;
    base.SerializeToString(&out);
    return QByteArray(out.data(), static_cast<int>(out.size()));
}

} // namespace

// ── RemoteVideoWidget ───────────────────────────────────────

RemoteVideoWidget::RemoteVideoWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(160, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    fps_timer_.start();
}

void RemoteVideoWidget::present_frame(const QVideoFrame& frame) {
    QMutexLocker lock(&frame_mutex_);
    QVideoFrame clone(frame);
    if (clone.map(QVideoFrame::ReadOnly)) {
        QImage::Format fmt = QVideoFrameFormat::imageFormatFromPixelFormat(clone.pixelFormat());
        if (fmt != QImage::Format_Invalid) {
            current_frame_ = QImage(clone.bits(0), clone.width(), clone.height(),
                                    clone.bytesPerLine(0), fmt).copy();
            current_size_ = current_frame_.size();
        } else {
            // 无法直接映射，用 QPainter 转换
            current_frame_ = QImage(clone.width(), clone.height(), QImage::Format_ARGB32);
            current_frame_.fill(Qt::black);
        }
        clone.unmap();
    }

    frame_count_++;
    update_fps();
    if (!video_on_) {
        video_on_ = true;
        emit first_frame_rendered();
    }
    update();
}

void RemoteVideoWidget::set_placeholder_text(const QString& text) {
    placeholder_text_ = text;
    if (!video_on_) update();
}

void RemoteVideoWidget::set_video_on(bool on) {
    video_on_ = on;
    if (!on) {
        current_frame_ = QImage();
    }
    update();
}

void RemoteVideoWidget::set_share_paused_hint(bool paused) {
    if (share_paused_hint_ == paused) return;
    share_paused_hint_ = paused;
    update();
}

void RemoteVideoWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (video_on_ && !current_frame_.isNull()) {
        QSize widget_size = size();
        QSize frame_size = current_frame_.size();
        if (!frame_size.isEmpty()) {
            QImage scaled = current_frame_.scaled(widget_size, Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation);
            int x = (widget_size.width() - scaled.width()) / 2;
            int y = (widget_size.height() - scaled.height()) / 2;
            painter.drawImage(x, y, scaled);
        }
        // #20 观看端：远端暂停共享时叠加半透明提示
        if (share_paused_hint_) {
            painter.fillRect(rect(), QColor(0, 0, 0, 140));
            QFont f = painter.font();
            f.setPointSize(14);
            f.setBold(true);
            painter.setFont(f);
            painter.setPen(QColor(0xFF, 0xD7, 0x00));
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("‖ 对方已暂停共享"));
        }
    } else {
        painter.fillRect(rect(), QColor(0x16, 0x21, 0x3e));
        if (!placeholder_text_.isEmpty()) {
            // 大号用户首字母
            QFont big_font = painter.font();
            big_font.setPointSize(48);
            big_font.setBold(true);
            painter.setFont(big_font);
            painter.setPen(QColor(0x4A, 0x90, 0xD9));
            QRect letter_rect(0, height() / 2 - 40, width(), 60);
            painter.drawText(letter_rect, Qt::AlignCenter,
                             placeholder_text_.mid(0, 1).toUpper());

            // "等待视频..." 小字
            QFont small_font = painter.font();
            small_font.setPointSize(11);
            painter.setFont(small_font);
            painter.setPen(QColor(0x88, 0x88, 0xaa));
            QRect wait_rect(0, height() / 2 + 10, width(), 24);
            painter.drawText(wait_rect, Qt::AlignCenter,
                             video_on_ ? "等待视频..." : "摄像头已关闭");
        }
    }
}

void RemoteVideoWidget::update_fps() {
    if (fps_timer_.elapsed() >= 1000) {
        fps_ = frame_count_ * 1000.0 / fps_timer_.restart();
        frame_count_ = 0;
        emit frame_rate_changed(fps_);
    }
}

// ── MediaEngine ─────────────────────────────────────────────

MediaEngine::MediaEngine(QObject* parent)
    : QObject(parent) {
    rtp_session_ = std::make_unique<RtpSession>(this);
    jitter_buffer_ = std::make_unique<JitterBuffer>(this);
    bandwidth_estimator_ = std::make_unique<BandwidthEstimator>(this);

    // 连接信号
    connect(rtp_session_.get(), &RtpSession::packet_received,
            this, &MediaEngine::on_rtp_packet_received);
    connect(bandwidth_estimator_.get(), &BandwidthEstimator::bandwidth_changed,
            this, &MediaEngine::on_bandwidth_estimated);
    connect(bandwidth_estimator_.get(), &BandwidthEstimator::quality_changed,
            this, &MediaEngine::on_quality_changed);

    // 统计定时器，每秒上报
    stats_timer_ = new QTimer(this);
    connect(stats_timer_, &QTimer::timeout, this, [this]() {
        if (rtp_session_) {
            auto s = rtp_session_->stats();
            // 更新带宽估计
            bandwidth_estimator_->report_loss(s.packet_loss_rate);
            bandwidth_estimator_->report_bitrate(s.bitrate_kbps);
            // #21：喂给屏幕共享控制器做三档自适应（本地发送侧丢包率）
            if (share_controller_ && screen_sharing_) {
                share_controller_->update_network_stats(s.packet_loss_rate);
            }
            emit stats_ready(s);

            // ★ 通过信令通道上报统计到服务器（触发服务端自适应带宽建议）
            if (user_id_ != 0 && !room_id_.isEmpty() && s.packets_sent > 0) {
                wemeet::MediaStatsReport report;
                report.set_user_id(user_id_);
                report.set_room_id(room_id_.toStdString());
                report.set_packet_loss_rate(s.packet_loss_rate);
                report.set_round_trip_time(s.rtt_ms);
                report.set_jitter_ms(s.jitter_ms);
                report.set_bitrate_kbps(s.bitrate_kbps);
                report.set_sent_bitrate_kbps(current_bandwidth());
                report.set_signal_quality(bandwidth_estimator_->network_quality());

                std::string payload;
                report.SerializeToString(&payload);
                emit signaling_message(make_signaling_msg(
                    static_cast<int>(wemeet::MSG_MEDIA_STATS_REPORT), payload));
            }
        }
        // 定期发送 RTCP SR
        if (rtp_session_->is_bound() && !relay_host_.isNull()) {
            rtp_session_->send_rtcp_sr(relay_host_, relay_video_port_);
        }
    });

    // 屏幕共享控制器（抓帧/帧差/自适应全部委托给它，#20-24）
    share_controller_ = new ScreenShareController(this);
    connect(share_controller_, &ScreenShareController::frame_ready,
            this, &MediaEngine::on_share_frame);
    connect(share_controller_, &ScreenShareController::state_changed,
            this, &MediaEngine::on_share_state_changed);
    connect(share_controller_, &ScreenShareController::source_invalidated,
            this, [this](const QString& reason) {
        // #24：源失效 → 暂停共享并通知 UI 提示用户重新选择
        send_screen_control(true);
        emit share_source_invalidated(reason);
    });
}

MediaEngine::~MediaEngine() {
    shutdown();
}

bool MediaEngine::initialize(uint64_t user_id, const QString& room_id) {
    if (initialized_) return true;

    user_id_ = user_id;
    room_id_ = room_id;
    initialized_ = true;

    // 绑定 RTP 端口（随机分配）
    if (!rtp_session_->bind(QHostAddress::AnyIPv4, 0)) {
        qWarning("MediaEngine: failed to bind RTP socket");
        return false;
    }

    // 缓存摄像头可用性
    camera_available_ = !QMediaDevices::videoInputs().isEmpty();
    if (!camera_available_) {
        // Qt 摄像头不可用时，检测 V4L2 设备
        std::vector<std::string> v4l2_devs;
        int n = V4L2Capture::enum_devices(v4l2_devs);
        camera_available_ = (n > 0);
    }

    stats_timer_->start(2000);  // 每2秒统计

    qDebug("MediaEngine initialized: user=%llu, room=%s, rtp_port=%u",
           user_id, qPrintable(room_id), rtp_session_->local_port());
    return true;
}

void MediaEngine::shutdown() {
    if (!initialized_) return;

    stats_timer_->stop();
    stop_camera();
    stop_microphone();
    stop_screen_share();
    rtp_session_->unbind();

    // 安全销毁所有远端 widget：解父子 + deleteLater（事件循环中清理）
    for (auto& rs : remote_streams_) {
        if (rs.widget) {
            rs.widget->setParent(nullptr);
            rs.widget->deleteLater();
            rs.widget = nullptr;
        }
    }
    remote_streams_.clear();
    initialized_ = false;
}

bool MediaEngine::start_camera(const QByteArray& camera_id) {
    if (camera_active_) return true;

    if (!camera_available_) {
        qWarning("MediaEngine: no camera available (cached)");
        return false;
    }

    // ★ Linux/WSL 下 V4L2 比 Qt Multimedia 更可靠（WSLg 经常找不到 Qt 摄像头）
    // 优先尝试 V4L2：它能直接读写 /dev/video*，帧通过 on_v4l2_frame_captured
    // 同时喂给本地预览和 RTP 发送
    if (try_v4l2_capture()) {
        return true;
    }
    qWarning("MediaEngine: V4L2 not available, falling back to Qt Multimedia");

    // ── Qt Multimedia 降级路径 ──
    QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        camera_available_ = false;
        qWarning("MediaEngine: no camera available (neither V4L2 nor Qt)");
        return false;
    }

    QCameraDevice device;
    if (!camera_id.isEmpty()) {
        for (auto& d : cameras) {
            device = d;
            break;
        }
    } else {
        device = cameras.first();
    }

    camera_ = std::make_unique<QCamera>(device, this);
    capture_session_ = std::make_unique<QMediaCaptureSession>();
    video_sink_ = std::make_unique<QVideoSink>(this);

    capture_session_->setCamera(camera_.get());
    capture_session_->setVideoSink(video_sink_.get());

    // 注：本地预览通过 RemoteVideoWidget::present_frame 由 on_video_frame_captured 完成
    // 不再依赖 capture_session_->setVideoOutput(QVideoWidget)

    connect(video_sink_.get(), &QVideoSink::videoFrameChanged,
            this, &MediaEngine::on_video_frame_captured);

    // 监听摄像头错误（独占冲突时会在此发出）
    connect(camera_.get(), &QCamera::errorOccurred,
            this, [this](QCamera::Error err, const QString& errorString) {
        qWarning("MediaEngine: camera error %d: %s",
                 static_cast<int>(err), qPrintable(errorString));
        camera_active_ = false;
        v4l2_active_ = false;
        emit error_occurred(QStringLiteral("摄像头无法访问: ") + errorString);
    });

    camera_->start();
    camera_active_ = true;

    if (!video_muted_) {
        start_capture_timer();
    }

    return true;
}

void MediaEngine::stop_camera() {
    if (v4l2_active_) {
        if (v4l2_capture_) {
            v4l2_capture_->stop();
        }
        v4l2_active_ = false;
    }

    if (camera_active_) {
        stop_capture_timer();
        if (camera_) {
            camera_->stop();
            camera_.reset();
        }
        capture_session_.reset();
        video_sink_.reset();
        camera_active_ = false;
    }
}

// ── V4L2 摄像头降级 ────────────────────────────────────────

bool MediaEngine::try_v4l2_capture() {
    if (v4l2_capture_) {
        // 已初始化过，直接启动
        v4l2_active_ = v4l2_capture_->start();
        return v4l2_active_;
    }

    // 查找 V4L2 设备
    std::vector<std::string> devices;
    if (V4L2Capture::enum_devices(devices) == 0) {
        qWarning("MediaEngine: no V4L2 devices found");
        return false;
    }

    // 尝试第一个设备
    v4l2_capture_ = std::make_unique<V4L2Capture>(this);

    // 连接帧信号
    connect(v4l2_capture_.get(), &V4L2Capture::frame_captured,
            this, &MediaEngine::on_v4l2_frame_captured);

    connect(v4l2_capture_.get(), &V4L2Capture::error_occurred,
            this, [this](const std::string& err) {
        qWarning("MediaEngine: V4L2 error: %s", err.c_str());
        emit error_occurred(QString::fromStdString(err));
    });

    if (!v4l2_capture_->open(devices[0], 640, 480, 30)) {
        v4l2_capture_.reset();
        return false;
    }

    v4l2_active_ = v4l2_capture_->start();
    return v4l2_active_;
}

// ── V4L2 帧回调 ────────────────────────────────────────────

void MediaEngine::on_v4l2_frame_captured(const QVideoFrame& frame) {
    emit local_video_frame(frame);

    // ★ 本地预览（与 Qt 路径共用 RemoteVideoWidget）
    if (local_preview_widget_ && !video_muted_) {
        local_preview_widget_->present_frame(frame);
    }

    // 发送到中继服务器
    if (rtp_session_->is_bound() && !relay_host_.isNull() && !video_muted_) {
        QVideoFrame clone(frame);
        if (clone.map(QVideoFrame::ReadOnly)) {
            QImage img(clone.bits(0), clone.width(), clone.height(),
                       QImage::Format_ARGB32);
            QByteArray jpeg_data;
            QBuffer buf(&jpeg_data);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "JPEG", 85);
            clone.unmap();

            rtp_session_->send_video_frame(jpeg_data, true, relay_host_, relay_video_port_);
            // 每 100 帧记录一次发送统计，方便排查流是否在出
            static std::atomic<uint64_t> v4l2_sent{0};
            uint64_t n = v4l2_sent.fetch_add(1);
            if (n % 100 == 0) {
                qDebug("MediaEngine: v4l2 sent %llu frames, dst=%s:%u, ssrc=%u, size=%lld",
                       n + 1, relay_host_.toString().toUtf8().constData(),
                       relay_video_port_, rtp_session_ ? rtp_session_->payload_type_video() : 0,
                       (long long)jpeg_data.size());
            }
        }
    }
}

bool MediaEngine::start_microphone() {
    if (mic_active_) return true;

    QAudioDevice input_device = QMediaDevices::defaultAudioInput();
    if (input_device.isNull()) {
        qWarning("MediaEngine: no audio input device");
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kAudioSampleRate);
    format.setChannelCount(kAudioChannels);
    format.setSampleFormat(QAudioFormat::Int16);

    audio_source_ = std::make_unique<QAudioSource>(input_device, format, this);
    audio_io_.reset(audio_source_->start());

    if (audio_io_) {
        mic_active_ = true;

        // 音量监测定时器（每100ms采样一次）
        if (!volume_monitor_timer_) {
            volume_monitor_timer_ = new QTimer(this);
            connect(volume_monitor_timer_, &QTimer::timeout, this, [this]() {
                if (!mic_active_ || audio_muted_ || !audio_io_) {
                    emit volume_level_changed(0.0);
                    return;
                }
                QByteArray buf = audio_io_->peek(audio_io_->bytesAvailable());
                if (buf.isEmpty()) {
                    emit volume_level_changed(0.0);
                    return;
                }
                // 计算 RMS 音量电平
                const int16_t* samples = reinterpret_cast<const int16_t*>(buf.constData());
                int count = buf.size() / 2;
                double sum = 0.0;
                for (int i = 0; i < count; ++i) {
                    double s = samples[i] / 32768.0;
                    sum += s * s;
                }
                double rms = std::sqrt(sum / std::max(1, count));
                double level = std::min(1.0, rms * 3.0); // 放大系数使视觉更明显
                emit volume_level_changed(level);
            });
        }
        volume_monitor_timer_->start(100);

        connect(audio_io_.get(), &QIODevice::readyRead, this, [this]() {
            if (!mic_active_ || audio_muted_ || !audio_io_) return;
            QByteArray audio_data = audio_io_->readAll();
            if (!audio_data.isEmpty() && rtp_session_->is_bound() && !relay_host_.isNull()) {
                rtp_session_->send_audio_frame(audio_data, relay_host_, relay_audio_port_);
            }
        });
        return true;
    }

    qWarning("MediaEngine: failed to start audio source");
    return false;
}

void MediaEngine::stop_microphone() {
    if (!mic_active_) return;
    if (audio_source_) {
        audio_source_->stop();
        audio_source_.reset();
    }
    audio_io_.reset();
    mic_active_ = false;
}

// ── 屏幕共享（委托 ScreenShareController，#19-24）────────────

bool MediaEngine::start_screen_share(const ShareSource& source) {
    // 正常共享中重复调用 → 忽略；暂停/源失效态允许换新源重启（#24）
    if (screen_sharing_ && share_controller_->state() == ShareState::Sharing) {
        return true;
    }

    if (!share_controller_->start(source)) {
        return false;   // 源无效（controller 已发 source_invalidated）
    }

    screen_sharing_ = true;
    qDebug("MediaEngine: screen sharing started (%s: %s)",
           source.is_window() ? "window" : "screen", qPrintable(source.title));
    return true;
}

bool MediaEngine::start_screen_share(int screen_index) {
    // 便捷重载：整屏共享（保持旧调用兼容）
    ShareSource source;
    source.type = ShareSource::Type::SCREEN;
    source.screen_index = screen_index;
    source.title = QStringLiteral("屏幕 %1").arg(screen_index + 1);
    return start_screen_share(source);
}

void MediaEngine::pause_screen_share() {
    if (!screen_sharing_) return;
    share_controller_->pause();
    send_screen_control(true);    // MediaControl{SCREEN, mute=true} 冻结标志
    emit screen_share_paused();
}

void MediaEngine::resume_screen_share() {
    if (!screen_sharing_) return;
    share_controller_->resume();
    send_screen_control(false);   // MediaControl{SCREEN, mute=false} 解冻
    emit screen_share_resumed();
}

void MediaEngine::stop_screen_share() {
    if (!screen_sharing_) return;
    share_controller_->stop();
    screen_sharing_ = false;
    qDebug("MediaEngine: screen sharing stopped");
}

/**
 * @brief 共享冻结标志信令（#20）
 *
 * 复用既有 MediaControl{media_type=SCREEN, mute=true/false} 通道，
 * 服务端收到后广播给房间内其他参与者，观看端据此叠加/解除
 * 「对方已暂停共享」提示。
 */
void MediaEngine::send_screen_control(bool mute) {
    if (user_id_ == 0 || room_id_.isEmpty()) return;

    wemeet::MediaControl ctrl;
    ctrl.set_target_user_id(user_id_);          // 控制对象 = 自己
    ctrl.set_room_id(room_id_.toStdString());
    ctrl.set_media_type(wemeet::MediaControl::SCREEN);
    ctrl.set_mute(mute);

    std::string payload;
    ctrl.SerializeToString(&payload);
    emit signaling_message(make_signaling_msg(
        static_cast<int>(wemeet::MSG_MEDIA_CONTROL), payload));
}

/**
 * @brief 共享帧到达：JPEG 编码（质量参数化，#21）+ RTP 发送
 */
void MediaEngine::on_share_frame(const QImage& frame, int jpeg_quality) {
    if (frame.isNull()) return;

    // ── 抓取有效性检测：WSLg/Wayland 下 grabWindow 会返回"非空但全黑"的 pixmap ──
    //   对小尺寸缩略图采样平均亮度，< 10 视为抓取失败
    bool frame_valid = true;
    {
        QPixmap probe = QPixmap::fromImage(frame).scaled(
            4, 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QImage img = probe.toImage().convertToFormat(QImage::Format_RGB32);
        quint64 sum = 0;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) sum += qGray(line[x]);
        }
        int pixels = img.width() * img.height();
        int avg = pixels > 0 ? static_cast<int>(sum / pixels) : 0;
        if (avg < 10) frame_valid = false;
    }

    if (!frame_valid) {
        // 全黑帧：跳过本地预览与发送（避免反复画全黑、浪费带宽）
        static std::atomic<uint64_t> invalid_count{0};
        uint64_t n = invalid_count.fetch_add(1);
        if (n == 0 || n % 50 == 0) {
            qWarning("MediaEngine: screen share frame is all-black (WSLg Wayland portal may be needed) #%llu",
                     n + 1);
        }
        return;
    }

    // ── 本地预览 ──
    if (local_preview_widget_) {
        QVideoFrame vf(frame);
        local_preview_widget_->present_frame(vf);
    }

    // ── 编码发送 ──
    if (relay_host_.isNull() || !rtp_session_->is_bound()) return;

    // ── 编码发送 ──
    if (relay_host_.isNull() || !rtp_session_->is_bound()) return;

    QByteArray screen_data;
    QBuffer buffer(&screen_data);
    buffer.open(QIODevice::WriteOnly);
    frame.save(&buffer, "JPEG", jpeg_quality);   // 质量由自适应档位决定

    rtp_session_->send_video_frame(screen_data, true, relay_host_, relay_video_port_);

    static std::atomic<uint64_t> screen_sent{0};
    uint64_t n = screen_sent.fetch_add(1);
    if (n % 50 == 0) {
        qDebug("MediaEngine: screen share sent %llu frames, %dx%d, q=%d, size=%lld",
               n + 1, frame.width(), frame.height(), jpeg_quality,
               (long long)screen_data.size());
    }
}

void MediaEngine::on_share_state_changed(ShareState state) {
    switch (state) {
    case ShareState::Sharing:
        if (!screen_sharing_) screen_sharing_ = true;
        emit screen_share_started();
        break;
    case ShareState::Idle:
        if (screen_sharing_) screen_sharing_ = false;
        emit screen_share_stopped();
        break;
    case ShareState::Paused:
        // 暂停/恢复的信令发送与信号在 pause/resume_screen_share() 中处理
        break;
    }
}

RemoteVideoWidget* MediaEngine::create_remote_video_widget(uint64_t user_id, QWidget* parent) {
    // 检查是否已存在
    for (auto& rs : remote_streams_) {
        if (rs.user_id == user_id) return rs.widget;
    }

    RemoteStream rs;
    rs.user_id = user_id;
    // 如果之前已有该用户的 SSRC，立即应用
    auto it = pending_peer_ssrc_.find(user_id);
    if (it != pending_peer_ssrc_.end()) {
        rs.ssrc = it->second;
        pending_peer_ssrc_.erase(it);
    }
    // 由 Qt 父-子对象系统独占管理生命周期
    rs.widget = new RemoteVideoWidget(parent);
    remote_streams_.push_back(rs);

    qDebug("MediaEngine: created remote video widget for user=%llu, ssrc=%u",
           user_id, rs.ssrc);
    return remote_streams_.back().widget;
}

void MediaEngine::remove_remote_video_widget(uint64_t user_id) {
    for (auto it = remote_streams_.begin(); it != remote_streams_.end(); ++it) {
        if (it->user_id == user_id) {
            // 先解父子关系，再用 deleteLater 在事件循环中安全销毁
            if (it->widget) {
                it->widget->setParent(nullptr);
                it->widget->deleteLater();
            }
            remote_streams_.erase(it);
            return;
        }
    }
}

void MediaEngine::clear_remote_video_widgets() {
    for (auto& rs : remote_streams_) {
        if (rs.widget) {
            rs.widget->setParent(nullptr);
            rs.widget->deleteLater();
            rs.widget = nullptr;
        }
    }
    remote_streams_.clear();
}

RemoteVideoWidget* MediaEngine::get_remote_widget(uint64_t user_id) const {
    for (auto& rs : remote_streams_) {
        if (rs.user_id == user_id) return rs.widget;
    }
    return nullptr;
}

void MediaEngine::set_relay_server(const QHostAddress& host, uint16_t video_port, uint16_t audio_port) {
    relay_host_ = host;
    relay_video_port_ = video_port;
    relay_audio_port_ = audio_port;
    qDebug("MediaEngine: relay = %s, video→port %u, audio→port %u",
           host.toString().toUtf8().constData(), video_port, audio_port);
}

void MediaEngine::set_ssrc(uint32_t video_ssrc, uint32_t audio_ssrc) {
    video_ssrc_ = video_ssrc;
    audio_ssrc_ = audio_ssrc;
    rtp_session_->set_ssrc(video_ssrc);
    qDebug("MediaEngine: RTP ssrc set = %u (video=%u, audio=%u)",
           video_ssrc, video_ssrc, audio_ssrc);
}

void MediaEngine::set_local_preview(RemoteVideoWidget* widget) {
    local_preview_widget_ = widget;
}

void MediaEngine::mute_audio(bool mute) {
    audio_muted_ = mute;
}

void MediaEngine::mute_video(bool mute) {
    video_muted_ = mute;
    if (mute) {
        stop_capture_timer();
    } else {
        start_capture_timer();
    }
}

uint32_t MediaEngine::current_bandwidth() const {
    return bandwidth_estimator_->estimated_bandwidth();
}

void MediaEngine::apply_bandwidth_hint(uint32_t max_bitrate_kbps) {
    if (!rtp_session_) return;

    // 节降：相同值不重复打印 / 不重复设置（避免高频 hint 反复刷屏与潜在写竞争）
    static std::atomic<uint32_t> last_applied{0};
    uint32_t prev = last_applied.load(std::memory_order_acquire);
    if (prev == max_bitrate_kbps) return;
    last_applied.store(max_bitrate_kbps, std::memory_order_release);

    rtp_session_->set_max_bitrate(max_bitrate_kbps);
    // 同步给本地带宽估计器，保证后续 stats 上报与建议一致
    bandwidth_estimator_->report_bitrate(max_bitrate_kbps);
    qDebug("MediaEngine: applied bandwidth hint = %u kbps", max_bitrate_kbps);
}

int32_t MediaEngine::network_quality() const {
    return bandwidth_estimator_->network_quality();
}

uint16_t MediaEngine::local_video_port() const {
    return rtp_session_ ? rtp_session_->local_port() : 0;
}

uint16_t MediaEngine::local_audio_port() const {
    return rtp_session_ ? rtp_session_->local_port() : 0;
}

void MediaEngine::set_peer_ssrc(uint64_t user_id, uint32_t ssrc) {
    for (auto& rs : remote_streams_) {
        if (rs.user_id == user_id) {
            rs.ssrc = ssrc;
            qDebug("MediaEngine: set peer user=%llu ssrc=%u", user_id, ssrc);
            return;
        }
    }
    // Widget 还没创建时，记录到待映射表
    pending_peer_ssrc_[user_id] = ssrc;
    qDebug("MediaEngine: queued peer ssrc for user=%llu ssrc=%u", user_id, ssrc);
}

RtpSession::Stats MediaEngine::get_stats() const {
    return rtp_session_->stats();
}

void MediaEngine::register_media_relay(const QString& relay_host, uint16_t relay_port) {
    set_relay_server(QHostAddress(relay_host), relay_port, relay_port + 1);
}

void MediaEngine::on_rtp_packet_received(const RTPPacket& packet,
                                          const QHostAddress& /*src*/, uint16_t /*port*/) {
    // 放入抖动缓冲（保留丢包统计；注意：JitterBuffer 为全局单队列，不区分 SSRC/PT，
    // 若在此 pop 排序会导致不同媒体流（video/audio/screen 各自独立 seq）混排错乱，
    // 因此处理仍走原始到达顺序，由 FrameAssembler 内部对乱序/缺包做健壮处理）
    jitter_buffer_->push_packet(packet);
    process_rtp_packet(packet);
}

void MediaEngine::process_rtp_packet(const RTPPacket& packet) {
    uint8_t pt = packet.payload_type();
    uint16_t seq = packet.sequence();
    uint32_t ts  = packet.timestamp_val();
    uint32_t ssrc = packet.ssrc();
    bool marker = packet.marker();

    if (pt == rtp_session_->payload_type_video()) {
        // ── 视频包：按 SSRC+timestamp 组装 JPEG 帧 ──
        auto it = frame_assemblers_.find(ssrc);
        if (it == frame_assemblers_.end() || !it->second.active ||
            it->second.timestamp != ts) {
            // 新帧开始
            FrameAssembler fa;
            fa.ssrc = ssrc;
            fa.timestamp = ts;
            fa.jpeg_data.reserve(64 * 1024);
            fa.jpeg_data = packet.payload;
            fa.expected_next_seq = static_cast<uint16_t>(seq + 1);
            fa.active = true;
            fa.last_update.start();
            frame_assemblers_[ssrc] = std::move(fa);
        } else {
            // 追加分片；先处理乱序/重复包（seq 回退或等于已处理序号 → 丢弃）
            if (seq < it->second.expected_next_seq) {
                // 乱序/重复包：序号已处理过，直接丢弃，避免死循环与数据错乱
                return;
            }
            if (seq > it->second.expected_next_seq) {
                // 中间缺包：本实现 FEC 恢复不可靠（recover_fec 返回的是 XOR 冗余而非原始数据），
                // 若继续拼接会导致 JPEG 数据错位 → 解码失败 → 黑屏。
                // 正确策略：标记本帧损坏，停止追加数据，等待 marker 到达后整体丢弃；
                // 下一帧（关键帧）会重新从首片开始组装，避免黑屏残影。
                it->second.missing_packets += static_cast<uint32_t>(seq - it->second.expected_next_seq);
                it->second.active = false;
                it->second.expected_next_seq = static_cast<uint16_t>(seq + 1);
            } else {
                // 序号连续：正常追加
                it->second.jpeg_data.append(packet.payload);
                it->second.expected_next_seq = static_cast<uint16_t>(seq + 1);
                it->second.last_update.restart();
            }
        }

        // 标记位 = 1 表示这是该帧的最后一个分片
        if (marker) {
            auto fa_it = frame_assemblers_.find(ssrc);
            if (fa_it != frame_assemblers_.end()) {
                QByteArray jpeg = fa_it->second.jpeg_data;
                bool complete = fa_it->second.active && fa_it->second.missing_packets == 0;
                frame_assemblers_.erase(fa_it);

                // 用 QImage::loadFromData 解码 JPEG
                QImage img;
                if (complete && img.loadFromData(jpeg, "JPEG") && !img.isNull()) {
                    QVideoFrame frame(img);
                    // 找到对应的远端 widget（优先按精确 ssrc 匹配）
                    RemoteVideoWidget* target_widget = nullptr;
                    uint64_t target_uid = 0;
                    for (auto& rs : remote_streams_) {
                        if (rs.ssrc == ssrc) {
                            target_widget = rs.widget;
                            target_uid = rs.user_id;
                            break;
                        }
                    }
                    // 尚未记录 ssrc 时，回退到第一个未设置的流
                    if (!target_widget) {
                        for (auto& rs : remote_streams_) {
                            if (rs.ssrc == 0) {
                                rs.ssrc = ssrc;
                                target_widget = rs.widget;
                                target_uid = rs.user_id;
                                break;
                            }
                        }
                    }
                    if (target_widget) {
                        target_widget->present_frame(frame);
                        // ★ 安全：emit 之前显式构造拷贝，避免栈对象跨信号连接
                        // （若接收方在跨线程 direct connection 中访问原 frame，
                        //   槽函数返回后 frame 析构 → use-after-free → segfault）
                        QVideoFrame frame_copy = frame;
                        emit remote_video_frame(target_uid, frame_copy);
                    }
                }
                // 不完整/解码失败的帧：整体丢弃，等待下一关键帧（避免黑屏残影）
            }
        }

        // 清理超时的重组缓冲（超过 1 秒未完成）
        for (auto it = frame_assemblers_.begin(); it != frame_assemblers_.end(); ) {
            if (it->second.last_update.elapsed() > 1000) {
                it = frame_assemblers_.erase(it);
            } else {
                ++it;
            }
        }
    } else if (pt == rtp_session_->payload_type_audio()) {
        // 音频包 — 透传（音频仍可后续扩展解码）
        emit remote_audio_data(packet.payload);
    }
}

void MediaEngine::on_bandwidth_estimated(uint32_t new_bitrate) {
    rtp_session_->set_max_bitrate(new_bitrate);
    emit bandwidth_changed(new_bitrate);
}

void MediaEngine::on_quality_changed(int32_t quality) {
    emit network_quality_changed(quality);
}

void MediaEngine::on_video_frame_captured(const QVideoFrame& frame) {
    emit local_video_frame(frame);

    // ★ 本地预览：直接 present_frame（与 V4L2 路径共用同一 widget）
    if (local_preview_widget_ && !video_muted_) {
        local_preview_widget_->present_frame(frame);
    }

    // 发送到中继服务器
    if (rtp_session_->is_bound() && !relay_host_.isNull() && !video_muted_) {
        QVideoFrame clone(frame);
        if (clone.map(QVideoFrame::ReadOnly)) {
            QImage img(clone.bits(0), clone.width(), clone.height(),
                       QImage::Format_ARGB32);
            QByteArray jpeg_data;
            QBuffer buf(&jpeg_data);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "JPEG", 85);  // 质量 85%
            clone.unmap();

            rtp_session_->send_video_frame(jpeg_data, true, relay_host_, relay_video_port_);
            // 每 100 帧记录一次发送统计
            static std::atomic<uint64_t> cam_sent{0};
            uint64_t n = cam_sent.fetch_add(1);
            if (n % 100 == 0) {
                qDebug("MediaEngine: camera sent %llu frames, dst=%s:%u, ssrc=%u, size=%lld",
                       n + 1, relay_host_.toString().toUtf8().constData(),
                       relay_video_port_,
                       rtp_session_ ? rtp_session_->payload_type_video() : 0,
                       (long long)jpeg_data.size());
            }
        }
    }
}

void MediaEngine::start_capture_timer() {
    if (!capture_timer_) {
        capture_timer_ = new QTimer(this);
        connect(capture_timer_, &QTimer::timeout, this, [this]() {
            // 定时器触发的帧捕获由 QVideoSink 的信号驱动
            // 此定时器仅用于保证发送速率
        });
    }
    capture_timer_->start(1000 / kVideoFps);
}

void MediaEngine::stop_capture_timer() {
    if (capture_timer_) {
        capture_timer_->stop();
    }
}
