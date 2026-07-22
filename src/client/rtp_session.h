#pragma once
#include <QObject>
#include <QUdpSocket>
#include <QElapsedTimer>
#include <QHostAddress>
#include <cstdint>
#include <vector>
#include <deque>
#include <functional>
#include <memory>
#include <atomic>
#include <arpa/inet.h>

/**
 * @brief RTP 媒体包头部（标准 RTPv2 12字节）
 */
#pragma pack(push, 1)
struct RTPHeader {
    uint8_t  cc_version;       // [0:3]=CSRC count, [4:7]=version=2
    uint8_t  pt_marker;        // [0:6]=payload type, [7]=marker
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

/**
 * @brief RTP 包（头部 + 负载）
 */
struct RTPPacket {
    RTPHeader header;
    QByteArray payload;

    uint8_t  payload_type() const { return header.pt_marker & 0x7F; }
    bool     marker() const { return (header.pt_marker & 0x80) != 0; }
    uint16_t sequence() const { return ntohs(header.sequence_number); }
    uint32_t timestamp_val() const { return ntohl(header.timestamp); }
    uint32_t ssrc() const { return ntohl(header.ssrc); }

    void set_payload_type(uint8_t pt) {
        header.pt_marker = (header.pt_marker & 0x80) | (pt & 0x7F);
    }
    void set_marker(bool m) {
        header.pt_marker = (header.pt_marker & 0x7F) | (m ? 0x80 : 0);
    }
    void set_sequence(uint16_t seq) { header.sequence_number = htons(seq); }
    void set_timestamp(uint32_t ts) { header.timestamp = htonl(ts); }
    void set_ssrc(uint32_t id) { header.ssrc = htonl(id); }

    QByteArray serialize() const {
        QByteArray data;
        data.append(reinterpret_cast<const char*>(&header), sizeof(RTPHeader));
        data.append(payload);
        return data;
    }

    static RTPPacket deserialize(const QByteArray& data) {
        RTPPacket pkt;
        if (data.size() < static_cast<int>(sizeof(RTPHeader))) return pkt;
        std::memcpy(&pkt.header, data.constData(), sizeof(RTPHeader));
        pkt.payload = data.mid(sizeof(RTPHeader));
        return pkt;
    }
};

/**
 * @brief RTCP Sender Report
 */
struct RTCPSenderReport {
    uint32_t ssrc;
    uint64_t ntp_timestamp;     // NTP 时间戳
    uint32_t rtp_timestamp;     // RTP 时间戳
    uint32_t packet_count;
    uint32_t octet_count;
};

/**
 * @brief RTCP Receiver Report 块
 */
struct RTCPReceiverBlock {
    uint32_t ssrc_source;
    uint8_t  fraction_lost;
    uint32_t cumulative_lost : 24;
    uint32_t highest_seqno;
    uint32_t jitter;
    uint32_t last_sr;
    uint32_t delay_since_last_sr;
};

/**
 * @brief RTP/UDP 会话 — 媒体传输层
 *
 * 负责：UDP 媒体包的发送/接收、RTP 序列化/反序列化、
 *       简单的丢包统计、FEC 前向纠错
 */
class RtpSession : public QObject {
    Q_OBJECT
public:
    explicit RtpSession(QObject* parent = nullptr);
    ~RtpSession();

    // ── 生命周期 ─────────────────────────────────────────
    bool bind(const QHostAddress& address, uint16_t port);
    void unbind();
    bool is_bound() const { return socket_ && socket_->state() == QAbstractSocket::BoundState; }

    // ── 参数配置 ─────────────────────────────────────────
    void set_ssrc(uint32_t ssrc) { ssrc_ = ssrc; }
    uint32_t ssrc() const { return ssrc_; }
    void set_payload_type_video(uint8_t pt) { pt_video_ = pt; }
    void set_payload_type_audio(uint8_t pt) { pt_audio_ = pt; }
    uint8_t payload_type_video() const { return pt_video_; }
    uint8_t payload_type_audio() const { return pt_audio_; }

    // ── 发送 ─────────────────────────────────────────────
    void send_video_frame(const QByteArray& frame_data, bool is_keyframe,
                          const QHostAddress& dest, uint16_t dest_port);
    void send_audio_frame(const QByteArray& audio_data,
                          const QHostAddress& dest, uint16_t dest_port);
    void send_rtcp_sr(const QHostAddress& dest, uint16_t dest_port);

    // ── 统计 ─────────────────────────────────────────────
    struct Stats {
        uint64_t packets_sent     = 0;
        uint64_t packets_received = 0;
        uint64_t packets_lost     = 0;
        uint64_t bytes_sent       = 0;
        uint64_t bytes_received   = 0;
        double   packet_loss_rate = 0.0;
        double   jitter_ms        = 0.0;
        double   rtt_ms           = 0.0;
        int32_t  signal_quality   = 5;
        uint32_t bitrate_kbps     = 0;
    };
    const Stats& stats() const { return stats_; }
    void reset_stats();

    // ── 自适应码率 ───────────────────────────────────────
    void set_max_bitrate(uint32_t kbps) { max_bitrate_kbps_ = kbps; }
    uint32_t max_bitrate() const { return max_bitrate_kbps_; }
    uint32_t current_bitrate() const { return current_bitrate_kbps_; }
    uint32_t suggest_bitrate() const;

    // ── 前向纠错 (FEC) ────────────────────────────────────
    void enable_fec(bool enable) { fec_enabled_ = enable; }
    bool fec_enabled() const { return fec_enabled_; }

    // ── 本地地址 ─────────────────────────────────────────
    QHostAddress local_address() const {
        return socket_ ? socket_->localAddress() : QHostAddress();
    }
    uint16_t local_port() const {
        return socket_ ? socket_->localPort() : 0;
    }

signals:
    void packet_received(const RTPPacket& packet, const QHostAddress& src, uint16_t src_port);
    void stats_updated(const Stats& stats);
    void quality_changed(int32_t quality);

private slots:
    void on_ready_read();

private:
    void update_stats();
    void send_packet(const RTPPacket& pkt, const QHostAddress& dest, uint16_t dest_port);

    // FEC: 简单异或奇偶校验
    QByteArray generate_fec(const QByteArray& data1, const QByteArray& data2);

    std::unique_ptr<QUdpSocket> socket_;
    uint32_t ssrc_ = 0;
    uint8_t  pt_video_ = 100;  // 动态负载类型
    uint8_t  pt_audio_ = 111;

    // 发送序列号
    uint16_t video_seq_ = 0;
    uint16_t audio_seq_ = 0;
    uint32_t video_ts_  = 0;
    uint32_t audio_ts_  = 0;

    // 统计
    Stats stats_;
    QElapsedTimer bitrate_timer_;
    uint64_t bytes_since_last_bitrate_ = 0;

    // 自适应
    uint32_t max_bitrate_kbps_ = 2500;
    uint32_t current_bitrate_kbps_ = 1500;

    // FEC
    bool fec_enabled_ = true;
    QByteArray last_video_packet_;  // 用于 FEC 奇偶校验
};

/**
 * @brief 抖动缓冲 — 网络抖动平滑处理
 *
 * 对收到的 RTP 包进行排序和缓冲，降低网络抖动影响
 */
class JitterBuffer : public QObject {
    Q_OBJECT
public:
    explicit JitterBuffer(QObject* parent = nullptr);

    void set_capacity(int ms) { capacity_ms_ = ms; }
    int  capacity_ms() const { return capacity_ms_; }

    void push_packet(const RTPPacket& packet);
    bool pop_packet(RTPPacket& packet);
    int  size() const { return static_cast<int>(buffer_.size()); }
    bool empty() const { return buffer_.empty(); }
    void clear();

    // 丢包检测
    uint32_t lost_packets() const { return lost_packets_; }
    double   loss_rate() const;

signals:
    void packet_ready();

private:
    int capacity_ms_ = 200;           // 200ms 抖动缓冲
    std::deque<RTPPacket> buffer_;
    uint16_t last_seqno_ = 0;
    bool     has_last_seqno_ = false;
    uint32_t lost_packets_ = 0;
    uint32_t total_packets_ = 0;
};

/**
 * @brief 带宽估计器 — 基于丢包率和延迟的自适应调整
 */
class BandwidthEstimator : public QObject {
    Q_OBJECT
public:
    explicit BandwidthEstimator(QObject* parent = nullptr);

    void report_loss(double loss_rate);
    void report_rtt(double rtt_ms);
    void report_bitrate(uint32_t bitrate_kbps);

    uint32_t estimated_bandwidth() const { return estimated_kbps_; }
    int32_t  network_quality() const { return quality_; } // 0-5, 5=最优

signals:
    void bandwidth_changed(uint32_t new_bitrate_kbps);
    void quality_changed(int32_t quality);

private:
    void recalculate();

    uint32_t estimated_kbps_ = 2500;
    int32_t  quality_ = 5;
    double   current_loss_ = 0.0;
    double   current_rtt_ = 0.0;

    // 平滑参数
    static constexpr double kLossAlpha = 0.3;
    static constexpr double kRttAlpha = 0.2;

    // 丢包率阈值
    static constexpr double kLossExcellent = 0.01;
    static constexpr double kLossGood = 0.03;
    static constexpr double kLossModerate = 0.05;
    static constexpr double kLossPoor = 0.10;

    // RTT 阈值
    static constexpr double kRttExcellent = 50.0;
    static constexpr double kRttGood = 100.0;
    static constexpr double kRttModerate = 200.0;
    static constexpr double kRttPoor = 400.0;
};
