#include "rtp_session.h"
#include <QtEndian>
#include <cmath>
#include <algorithm>
#include <cstring>

// ── RtpSession ──────────────────────────────────────────────

RtpSession::RtpSession(QObject* parent)
    : QObject(parent)
    , socket_(std::make_unique<QUdpSocket>(this)) {
    bitrate_timer_.start();
    connect(socket_.get(), &QUdpSocket::readyRead, this, &RtpSession::on_ready_read);
}

RtpSession::~RtpSession() {
    unbind();
}

bool RtpSession::bind(const QHostAddress& address, uint16_t port) {
    if (socket_->state() == QAbstractSocket::BoundState) {
        socket_->close();
    }
    bool ok = socket_->bind(address, port);
    if (!ok) {
        qWarning("RtpSession: bind failed on %s:%u — %s",
                 qPrintable(address.toString()), port,
                 qPrintable(socket_->errorString()));
    }
    return ok;
}

void RtpSession::unbind() {
    if (socket_->state() == QAbstractSocket::BoundState) {
        socket_->close();
    }
}

void RtpSession::send_video_frame(const QByteArray& frame_data, bool is_keyframe,
                                   const QHostAddress& dest, uint16_t dest_port) {
    // 大帧分片发送 (MTU ~1400)
    constexpr int kMTU = 1400;
    int offset = 0;
    int frame_size = frame_data.size();
    int total_packets = (frame_size + kMTU - 1) / kMTU;

    for (int i = 0; i < total_packets; ++i) {
        int chunk_size = std::min(kMTU, frame_size - offset);

        RTPPacket pkt;
        std::memset(&pkt.header, 0, sizeof(RTPHeader));
        pkt.header.cc_version = (2 << 6);  // version=2
        pkt.set_payload_type(pt_video_);
        pkt.set_marker(is_keyframe && (i == total_packets - 1));
        pkt.set_sequence(video_seq_++);
        pkt.set_timestamp(video_ts_);
        pkt.set_ssrc(ssrc_);
        pkt.payload = frame_data.mid(offset, chunk_size);

        send_packet(pkt, dest, dest_port);
        offset += chunk_size;
    }

    // 帧率约 30fps → 每帧 3000 时钟增量
    video_ts_ += 3000;

    // 更新码率统计
    bytes_since_last_bitrate_ += frame_data.size();
    stats_.packets_sent += total_packets;
}

void RtpSession::send_audio_frame(const QByteArray& audio_data,
                                   const QHostAddress& dest, uint16_t dest_port) {
    constexpr int kAudioMTU = 512;

    for (int offset = 0; offset < audio_data.size(); offset += kAudioMTU) {
        int chunk = std::min<int>(kAudioMTU, static_cast<int>(audio_data.size() - offset));

        RTPPacket pkt;
        std::memset(&pkt.header, 0, sizeof(RTPHeader));
        pkt.header.cc_version = (2 << 6);
        pkt.set_payload_type(pt_audio_);
        pkt.set_marker(offset == 0);
        pkt.set_sequence(audio_seq_++);
        pkt.set_timestamp(audio_ts_);
        pkt.set_ssrc(ssrc_);
        pkt.payload = audio_data.mid(offset, chunk);

        send_packet(pkt, dest, dest_port);
    }

    // 48kHz, 20ms 每帧 → 960 采样/帧
    audio_ts_ += 960;
    bytes_since_last_bitrate_ += audio_data.size();
    stats_.packets_sent++;
}

void RtpSession::send_rtcp_sr(const QHostAddress& dest, uint16_t dest_port) {
    // 简化 RTCP Sender Report — 用于统计
    // 实际应包含 NTP 时间戳和包/字节计数
    RTPPacket sr_pkt;
    std::memset(&sr_pkt.header, 0, sizeof(RTPHeader));
    sr_pkt.header.cc_version = (2 << 6);
    sr_pkt.set_payload_type(200);  // RTCP SR
    sr_pkt.set_ssrc(ssrc_);

    QByteArray sr_body;
    // SR: SSRC + NTP ts(8) + RTP ts(4) + pkt count(4) + octet count(4)
    uint32_t ssrc_be = qToBigEndian(ssrc_);
    sr_body.append(reinterpret_cast<char*>(&ssrc_be), 4);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count();
    uint64_t ntp = (static_cast<uint64_t>(ms / 1000) + 2208988800ULL) << 32;
    ntp |= (ms % 1000) * 0x100000000ULL / 1000;
    uint64_t ntp_be = qToBigEndian(ntp);
    sr_body.append(reinterpret_cast<char*>(&ntp_be), 8);

    uint32_t ts_be = qToBigEndian(video_ts_);
    sr_body.append(reinterpret_cast<char*>(&ts_be), 4);
    uint32_t pc_be = qToBigEndian(static_cast<uint32_t>(stats_.packets_sent));
    sr_body.append(reinterpret_cast<char*>(&pc_be), 4);
    uint32_t oc_be = qToBigEndian(static_cast<uint32_t>(bytes_since_last_bitrate_));
    sr_body.append(reinterpret_cast<char*>(&oc_be), 4);

    sr_pkt.payload = sr_body;
    send_packet(sr_pkt, dest, dest_port);
}

void RtpSession::send_packet(const RTPPacket& pkt,
                              const QHostAddress& dest, uint16_t dest_port) {
    QByteArray data = pkt.serialize();
    socket_->writeDatagram(data, dest, dest_port);
    stats_.packets_sent++;
    stats_.bytes_sent += data.size();
    update_stats();
}

void RtpSession::on_ready_read() {
    while (socket_->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_->pendingDatagramSize()));
        QHostAddress src_addr;
        uint16_t src_port;

        socket_->readDatagram(datagram.data(), datagram.size(), &src_addr, &src_port);

        if (datagram.size() < static_cast<int>(sizeof(RTPHeader))) continue;

        RTPPacket pkt = RTPPacket::deserialize(datagram);
        stats_.packets_received++;
        stats_.bytes_received += datagram.size();

        // RTCP 包
        if (pkt.payload_type() >= 200) {
            // 解析 RTCP — 这里简化处理
            continue;
        }

        // RTP 媒体包
        emit packet_received(pkt, src_addr, src_port);
        update_stats();
    }
}

void RtpSession::update_stats() {
    // 每秒更新一次码率统计
    if (bitrate_timer_.elapsed() >= 1000) {
        current_bitrate_kbps_ = static_cast<uint32_t>(
            (bytes_since_last_bitrate_ * 8) / std::max<qint64>(1, bitrate_timer_.elapsed()));
        bitrate_timer_.restart();
        bytes_since_last_bitrate_ = 0;

        // 丢包率
        uint64_t total = stats_.packets_sent + stats_.packets_received;
        if (total > 0) {
            stats_.packet_loss_rate = static_cast<double>(stats_.packets_lost) / total;
        }

        emit stats_updated(stats_);
    }
}

void RtpSession::reset_stats() {
    stats_ = Stats{};
    video_seq_ = 0;
    audio_seq_ = 0;
    video_ts_ = 0;
    audio_ts_ = 0;
    bytes_since_last_bitrate_ = 0;
}

uint32_t RtpSession::suggest_bitrate() const {
    double loss = stats_.packet_loss_rate;
    if (loss > 0.15) return 300;
    if (loss > 0.10) return 500;
    if (loss > 0.05) return 1000;
    if (loss > 0.02) return 1500;
    return max_bitrate_kbps_;
}

QByteArray RtpSession::generate_fec(const QByteArray& d1, const QByteArray& d2) {
    QByteArray fec;
    size_t max_len = std::max(d1.size(), d2.size());
    fec.resize(max_len);
    for (size_t i = 0; i < max_len; ++i) {
        uint8_t b1 = i < static_cast<size_t>(d1.size()) ? static_cast<uint8_t>(d1[i]) : 0;
        uint8_t b2 = i < static_cast<size_t>(d2.size()) ? static_cast<uint8_t>(d2[i]) : 0;
        fec[i] = static_cast<char>(b1 ^ b2);
    }
    return fec;
}

// ── JitterBuffer ────────────────────────────────────────────

JitterBuffer::JitterBuffer(QObject* parent) : QObject(parent) {}

void JitterBuffer::push_packet(const RTPPacket& packet) {
    uint16_t seq = packet.sequence();
    total_packets_++;

    if (!has_last_seqno_) {
        has_last_seqno_ = true;
        last_seqno_ = seq;
        buffer_.push_back(packet);
        emit packet_ready();
        return;
    }

    // 检测丢包
    if (seq > last_seqno_ + 1) {
        lost_packets_ += (seq - last_seqno_ - 1);
    }
    last_seqno_ = seq;

    // 按序列号排序插入
    auto it = buffer_.begin();
    while (it != buffer_.end() && it->sequence() < seq) {
        ++it;
    }
    buffer_.insert(it, packet);

    // 限制缓冲区大小
    if (buffer_.size() > 500) {
        buffer_.pop_front();
    }
}

bool JitterBuffer::pop_packet(RTPPacket& packet) {
    if (buffer_.empty()) return false;
    packet = buffer_.front();
    buffer_.pop_front();
    return true;
}

void JitterBuffer::clear() {
    buffer_.clear();
    has_last_seqno_ = false;
    last_seqno_ = 0;
    lost_packets_ = 0;
    total_packets_ = 0;
}

double JitterBuffer::loss_rate() const {
    if (total_packets_ == 0) return 0.0;
    return static_cast<double>(lost_packets_) / total_packets_;
}

// ── BandwidthEstimator ──────────────────────────────────────

BandwidthEstimator::BandwidthEstimator(QObject* parent) : QObject(parent) {}

void BandwidthEstimator::report_loss(double loss_rate) {
    current_loss_ = (1.0 - kLossAlpha) * current_loss_ + kLossAlpha * loss_rate;
    recalculate();
}

void BandwidthEstimator::report_rtt(double rtt_ms) {
    current_rtt_ = (1.0 - kRttAlpha) * current_rtt_ + kRttAlpha * rtt_ms;
    recalculate();
}

void BandwidthEstimator::report_bitrate(uint32_t bitrate_kbps) {
    recalculate();
}

void BandwidthEstimator::recalculate() {
    int32_t new_quality = 5;
    uint32_t new_bw = estimated_kbps_;

    // 基于丢包率
    if (current_loss_ > kLossPoor) {
        new_quality = 1;
        new_bw = std::min(new_bw, 500u);
    } else if (current_loss_ > kLossModerate) {
        new_quality = 2;
        new_bw = std::min(new_bw, 1000u);
    } else if (current_loss_ > kLossGood) {
        new_quality = 3;
        new_bw = std::min(new_bw, 1500u);
    } else if (current_loss_ > kLossExcellent) {
        new_quality = 4;
        new_bw = std::min(new_bw, 2000u);
    } else {
        new_quality = 5;
        new_bw = 2500u;
    }

    // 基于 RTT
    if (current_rtt_ > kRttPoor) {
        new_quality = std::min(new_quality, 1);
        new_bw = new_bw * 0.5;
    } else if (current_rtt_ > kRttModerate) {
        new_quality = std::min(new_quality, 2);
        new_bw = new_bw * 0.7;
    } else if (current_rtt_ > kRttGood) {
        new_quality = std::min(new_quality, 3);
        new_bw = new_bw * 0.85;
    }

    // 确保最小带宽
    new_bw = std::max(new_bw, 200u);
    new_bw = std::min(new_bw, 5000u);

    if (new_quality != quality_) {
        quality_ = new_quality;
        emit quality_changed(quality_);
    }

    if (new_bw != estimated_kbps_) {
        estimated_kbps_ = new_bw;
        emit bandwidth_changed(estimated_kbps_);
    }
}
