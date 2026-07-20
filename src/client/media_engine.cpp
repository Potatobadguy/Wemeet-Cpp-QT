#include "media_engine.h"
#include "rtp_session.h"
#include <QVideoFrame>
#include <cmath>
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
            emit stats_ready(s);
        }
        // 定期发送 RTCP SR
        if (rtp_session_->is_bound() && !relay_host_.isNull()) {
            rtp_session_->send_rtcp_sr(relay_host_, relay_video_port_);
        }
    });

    // 屏幕共享定时器
    screen_capture_timer_ = new QTimer(this);
    connect(screen_capture_timer_, &QTimer::timeout, this, &MediaEngine::capture_screen_frame);
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
    if (camera_available_) {
        qDebug("MediaEngine: Qt camera available");
    } else {
        // Qt 摄像头不可用时，检测 V4L2 设备
        std::vector<std::string> v4l2_devs;
        int n = V4L2Capture::enum_devices(v4l2_devs);
        camera_available_ = (n > 0);
        if (camera_available_) {
            qDebug("MediaEngine: V4L2 fallback available (%d device(s))", n);
        } else {
            qDebug("MediaEngine: no camera available");
        }
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

    remote_streams_.clear();
    initialized_ = false;
}

bool MediaEngine::start_camera(const QByteArray& camera_id) {
    if (camera_active_) return true;

    if (!camera_available_) {
        qWarning("MediaEngine: no camera available (cached)");
        return false;
    }

    QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        camera_available_ = false;
        qWarning("MediaEngine: no camera available");
        return false;
    }

    QCameraDevice device;
    if (!camera_id.isEmpty()) {
        for (auto& d : cameras) {
            // 使用第一个可用设备作为简化
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

    // 重新连接本地预览控件（关闭摄像头再开启后需要重连）
    if (local_preview_widget_) {
        capture_session_->setVideoOutput(local_preview_widget_);
    }

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
    qDebug("MediaEngine: camera started");

    if (!video_muted_) {
        // 开始发送视频帧的定时器
        start_capture_timer();
    }

    // ── V4L2 降级 ────────────────────────────────────────
    // Qt Multimedia 摄像头不可用时，直接通过 V4L2 访问 /dev/video*
    if (!camera_active_) {
        qDebug("MediaEngine: trying V4L2 fallback...");
        if (try_v4l2_capture()) {
            qDebug("MediaEngine: V4L2 camera started via %s",
                   v4l2_capture_->device_name().c_str());

            if (!video_muted_) {
                start_capture_timer();
            }
            return true;
        }
        qWarning("MediaEngine: V4L2 fallback also failed");
    }

    return true;
}

void MediaEngine::stop_camera() {
    if (v4l2_active_) {
        if (v4l2_capture_) {
            v4l2_capture_->stop();
        }
        v4l2_active_ = false;
        qDebug("MediaEngine: V4L2 camera stopped");
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
        qDebug("MediaEngine: camera stopped");
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

    qDebug("MediaEngine: found %zu V4L2 device(s)", devices.size());
    for (size_t i = 0; i < devices.size(); ++i) {
        qDebug("  [%zu] %s", i, devices[i].c_str());
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
        qDebug("MediaEngine: microphone started");
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
    qDebug("MediaEngine: microphone stopped");
}

bool MediaEngine::start_screen_share(int screen_index) {
    if (screen_sharing_) return true;

    QList<QScreen*> screens = QGuiApplication::screens();
    if (screen_index < 0 || screen_index >= screens.size()) {
        qWarning("MediaEngine: invalid screen index %d", screen_index);
        return false;
    }

    screen_index_ = screen_index;
    screen_sharing_ = true;

    // 每秒 5 帧的屏幕共享
    screen_capture_timer_->start(200);
    emit screen_share_started();
    qDebug("MediaEngine: screen sharing started (screen %d)", screen_index);
    return true;
}

void MediaEngine::stop_screen_share() {
    if (!screen_sharing_) return;
    screen_capture_timer_->stop();
    screen_sharing_ = false;
    emit screen_share_stopped();
    qDebug("MediaEngine: screen sharing stopped");
}

void MediaEngine::capture_screen_frame() {
    if (!screen_sharing_ || relay_host_.isNull()) return;

    QList<QScreen*> screens = QGuiApplication::screens();
    if (screen_index_ >= screens.size()) return;

    QScreen* screen = screens[screen_index_];
    if (!screen) return;

    QPixmap pixmap = screen->grabWindow(0);
    QByteArray screen_data;
    QBuffer buffer(&screen_data);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "JPEG", 70); // JPEG 压缩控制质量

    if (rtp_session_->is_bound()) {
        rtp_session_->send_video_frame(screen_data, true, relay_host_, relay_video_port_);
    }
}

RemoteVideoWidget* MediaEngine::create_remote_video_widget(uint64_t user_id, QWidget* parent) {
    // 检查是否已存在
    for (auto& rs : remote_streams_) {
        if (rs.user_id == user_id) return rs.widget.get();
    }

    RemoteStream rs;
    rs.user_id = user_id;
    // 如果之前已有该用户的 SSRC，立即应用
    auto it = pending_peer_ssrc_.find(user_id);
    if (it != pending_peer_ssrc_.end()) {
        rs.ssrc = it->second;
        pending_peer_ssrc_.erase(it);
    }
    rs.widget = std::make_unique<RemoteVideoWidget>(parent);
    remote_streams_.push_back(std::move(rs));

    qDebug("MediaEngine: created remote video widget for user=%llu, ssrc=%u",
           user_id, rs.ssrc);
    return remote_streams_.back().widget.get();
}

void MediaEngine::remove_remote_video_widget(uint64_t user_id) {
    remote_streams_.erase(
        std::remove_if(remote_streams_.begin(), remote_streams_.end(),
                        [user_id](const RemoteStream& rs) { return rs.user_id == user_id; }),
        remote_streams_.end());
}

RemoteVideoWidget* MediaEngine::get_remote_widget(uint64_t user_id) const {
    for (auto& rs : remote_streams_) {
        if (rs.user_id == user_id) return rs.widget.get();
    }
    return nullptr;
}

void MediaEngine::set_relay_server(const QHostAddress& host, uint16_t video_port, uint16_t audio_port) {
    relay_host_ = host;
    relay_video_port_ = video_port;
    relay_audio_port_ = audio_port;
}

void MediaEngine::set_ssrc(uint32_t video_ssrc, uint32_t audio_ssrc) {
    video_ssrc_ = video_ssrc;
    audio_ssrc_ = audio_ssrc;
    rtp_session_->set_ssrc(video_ssrc);
}

void MediaEngine::set_local_preview(QVideoWidget* widget) {
    local_preview_widget_ = widget;
    if (capture_session_ && widget) {
        capture_session_->setVideoOutput(widget);
    }
}

void MediaEngine::mute_audio(bool mute) {
    audio_muted_ = mute;
    qDebug("MediaEngine: audio %s", mute ? "muted" : "unmuted");
}

void MediaEngine::mute_video(bool mute) {
    video_muted_ = mute;
    if (mute) {
        stop_capture_timer();
    } else {
        start_capture_timer();
    }
    qDebug("MediaEngine: video %s", mute ? "muted" : "unmuted");
}

uint32_t MediaEngine::current_bandwidth() const {
    return bandwidth_estimator_->estimated_bandwidth();
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
    // 放入抖动缓冲（保留重排序与丢包统计）
    jitter_buffer_->push_packet(packet);

    uint8_t pt = packet.payload_type();
    uint16_t seq = packet.sequence();
    uint32_t ts  = packet.timestamp_val();
    uint32_t ssrc = packet.ssrc();
    bool marker = packet.marker();

    if (pt > 96) {
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
            // 追加分片
            it->second.jpeg_data.append(packet.payload);
            it->second.expected_next_seq = static_cast<uint16_t>(seq + 1);
            it->second.last_update.restart();
        }

        // 标记位 = 1 表示这是该帧的最后一个分片
        if (marker) {
            auto fa_it = frame_assemblers_.find(ssrc);
            if (fa_it != frame_assemblers_.end()) {
                QByteArray jpeg = fa_it->second.jpeg_data;
                frame_assemblers_.erase(fa_it);

                // 用 QImage::loadFromData 解码 JPEG
                QImage img;
                if (img.loadFromData(jpeg, "JPEG") && !img.isNull()) {
                    QVideoFrame frame(img);
                    // 找到对应的远端 widget
                    for (auto& rs : remote_streams_) {
                        if (rs.ssrc == ssrc || rs.ssrc == 0) {
                            if (rs.ssrc == 0) rs.ssrc = ssrc;  // 记录
                            rs.widget->present_frame(frame);
                            emit remote_video_frame(rs.user_id, frame);
                            break;
                        }
                    }
                }
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
    } else {
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
