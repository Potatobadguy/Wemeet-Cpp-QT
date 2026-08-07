#pragma once
#include <QObject>
#include <QUdpSocket>
#include <QElapsedTimer>
#include <QHostAddress>
#include <cstdint>
#include <vector>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <atomic>
#if defined(_WIN32)
#  include <winsock2.h>   // htons/htonl/ntohs/ntohl（Windows 下由 winsock2.h 提供）
#else
#  include <arpa/inet.h>  // htons/htonl/ntohs/ntohl（POSIX 平台）
#endif
#include "rtp_header.h"   // common/rtp_header.h — RTPHeader 唯一定义（#9）

// RTPHeader 唯一定义在 src/common/rtp_header.h（namespace wemeet），
// 此处 using 保持既有客户端代码零改动（RTPPacket 等直接引用 RTPHeader）。
using wemeet::RTPHeader;

/**
 * @brief RTP 包（头部 + 负载）
 *
 * 提供便捷的字段访问（自动做网络/主机字节序转换）与
 * 序列化/反序列化，供 QUdpSocket 直接读写。
 */
struct RTPPacket {
    RTPHeader header;
    QByteArray payload;

    // ── 解码访问（网络序 → 主机序）────────────────────
    uint8_t  payload_type() const { return header.pt_marker & 0x7F; }
    bool     marker() const { return (header.pt_marker & 0x80) != 0; }
    uint16_t sequence() const { return ntohs(header.sequence_number); }
    uint32_t timestamp_val() const { return ntohl(header.timestamp); }
    uint32_t ssrc() const { return ntohl(header.ssrc); }

    // ── 编码设置（主机序 → 网络序）────────────────────
    void set_payload_type(uint8_t pt) {
        header.pt_marker = (header.pt_marker & 0x80) | (pt & 0x7F);
    }
    void set_marker(bool m) {
        header.pt_marker = (header.pt_marker & 0x7F) | (m ? 0x80 : 0);
    }
    void set_sequence(uint16_t seq) { header.sequence_number = htons(seq); }
    void set_timestamp(uint32_t ts) { header.timestamp = htonl(ts); }
    void set_ssrc(uint32_t id) { header.ssrc = htonl(id); }

    // ── 序列化 ─────────────────────────────────────────
    QByteArray serialize() const {   // 头部 + 负载拼成完整字节流
        QByteArray data;
        data.append(reinterpret_cast<const char*>(&header), sizeof(RTPHeader));
        data.append(payload);
        return data;
    }

    static RTPPacket deserialize(const QByteArray& data) {   // 从字节流还原
        RTPPacket pkt;
        // #17 反序列化前校验：包长 >= 12 且 RTP version == 2，
        // 非法包返回空 RTPPacket（payload 为空，调用方据此丢弃）
        if (!wemeet::validate_rtp_packet(
                reinterpret_cast<const uint8_t*>(data.constData()),
                static_cast<size_t>(data.size()))) {
            return pkt;
        }
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
 *
 * 发送路径：
 *   send_video_frame / send_audio_frame（分片 + seq/ts 递增）
 *     → send_packet → QUdpSocket::writeDatagram → 中继/对端
 * 接收路径：
 *   QUdpSocket::readyRead → on_ready_read → emit packet_received
 *     → 上层（MediaEngine）送入 JitterBuffer 排序缓冲后渲染
 */
class RtpSession : public QObject {
    Q_OBJECT
public:
    explicit RtpSession(QObject* parent = nullptr);
    ~RtpSession();

    // ── 生命周期 ─────────────────────────────────────────
    /**
     * @brief 绑定本地 UDP 地址端口（媒体接收端口）
     */
    bool bind(const QHostAddress& address, uint16_t port);
    void unbind();
    bool is_bound() const { return socket_ && socket_->state() == QAbstractSocket::BoundState; }

    // ── 参数配置 ─────────────────────────────────────────
    void set_ssrc(uint32_t ssrc) { ssrc_ = ssrc; }   // 设置本会话 SSRC
    uint32_t ssrc() const { return ssrc_; }
    void set_payload_type_video(uint8_t pt) { pt_video_ = pt; }   // 动态负载类型
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

    // FEC 负载类型（用于识别冗余包，动态范围内不与 video/audio 冲突）
    void set_payload_type_fec(uint8_t pt) { pt_fec_ = pt; }
    uint8_t payload_type_fec() const { return pt_fec_; }

    // 接收侧：根据缺失的媒体 seq 尝试从 FEC 缓存还原数据
    // 返回 true 表示成功还原，data 为还原出的负载
    bool recover_fec(uint16_t lost_seq, QByteArray& data);

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
    void update_stats();   // 每秒更新码率/丢包统计
    void send_packet(const RTPPacket& pkt, const QHostAddress& dest, uint16_t dest_port);   // 单包发送 + 统计

    // FEC: 简单异或奇偶校验
    QByteArray generate_fec(const QByteArray& data1, const QByteArray& data2);

    std::unique_ptr<QUdpSocket> socket_;   // UDP 媒体 socket（信号驱动）
    uint32_t ssrc_ = 0;                    // 本会话 SSRC（媒体流唯一标识）
    uint8_t  pt_video_ = 100;              // 视频动态负载类型
    uint8_t  pt_audio_ = 111;              // 音频动态负载类型（标准 PCMA/PCMU 常用）
    uint8_t  pt_fec_   = 124;              // FEC 冗余包负载类型（动态范围内保留）

    // 发送序列号/时间戳
    uint16_t video_seq_ = 0;   // 视频包序号（递增，检测丢包）
    uint16_t audio_seq_ = 0;   // 音频包序号
    uint16_t fec_seq_   = 0;   // FEC 包独立序号
    uint32_t video_ts_  = 0;   // 视频时间戳（30fps → 每帧 +3000）
    uint32_t audio_ts_  = 0;   // 音频时间戳（48kHz → 20ms/帧 +960）

    // 统计
    Stats stats_;
    QElapsedTimer bitrate_timer_;   // 码率计算计时器
    uint64_t bytes_since_last_bitrate_ = 0;   // 上次统计以来的字节数

    // 自适应
    uint32_t max_bitrate_kbps_ = 2500;       // 码率上限
    uint32_t current_bitrate_kbps_ = 1500;   // 当前实际码率

    // FEC
    bool fec_enabled_ = true;
    QByteArray last_video_packet_;  // 缓存上一视频包，用于 FEC 异或奇偶校验

    // 接收侧 FEC 缓存: 被保护起始 seq → 冗余记录
    // 冗余包负载格式: [2B 保护起始seq][2B 保护结束seq][XOR data]
    struct FecRecord {
        uint16_t   start_seq;
        uint16_t   end_seq;
        QByteArray xor_data;
    };
    std::map<uint16_t, FecRecord> fec_cache_;

    // FEC 发送辅助：为每帧生成并发送冗余包
    void send_video_fec(const QByteArray& first_payload, int frame_packets,
                        const QHostAddress& dest, uint16_t dest_port);

    // 接收侧：解析 FEC 冗余包并缓存
    void parse_fec_packet(const RTPPacket& pkt);

    // 接收侧：解析 RTCP Sender Report，估算 RTT
    void parse_rtcp_packet(const QByteArray& datagram);
};

/**
 * @brief 抖动缓冲 — 网络抖动平滑处理
 *
 * ─────────────────────────────────────────────────────────────
 *  200ms 抖动缓冲的工作方式
 * ─────────────────────────────────────────────────────────────
 *  作用：UDP 网络抖动会导致 RTP 包到达时间忽早忽晚、甚至乱序。
 *  JitterBuffer 用一个【缓冲 + 排序】的中间层吸收这种抖动：
 *
 *   到达的乱序包 ──► push_packet 按 seq 升序插入 ──► 排序队列
 *   播放器定时 pop_packet ──► 永远取 seq 最小的包 ──► 有序流出
 *
 *  "200ms" = capacity_ms_ 缓冲深度：
 *   - 播放器故意比真实到达滞后最多 200ms，让早到的包等待、
 *     晚到的包赶上，从而抹平到达时间的波动；
 *   - 代价是增加最多 200ms 的端到端延迟（【延迟换平滑】），
 *     因此只用于接收/播放端；
 *   - 若抖动超过 200ms，缓冲填满后仍会丢包，这由丢包检测统计
 *     并交给带宽估计器触发降码率。
 *
 *  补充：capacity_ms_ 是"语义深度"，实际实现用缓冲包数量上限
 *  （500 包）做物理保护，防止异常时内存膨胀。
 */
class JitterBuffer : public QObject {
    Q_OBJECT
public:
    explicit JitterBuffer(QObject* parent = nullptr);

    void set_capacity(int ms) { capacity_ms_ = ms; }
    int  capacity_ms() const { return capacity_ms_; }

    void push_packet(const RTPPacket& packet);   // 入队（按 seq 排序插入）
    bool pop_packet(RTPPacket& packet);          // 出队（取 seq 最小者）
    int  size() const { return static_cast<int>(buffer_.size()); }
    bool empty() const { return buffer_.empty(); }
    void clear();

    // 丢包检测
    uint32_t lost_packets() const { return lost_packets_; }
    double   loss_rate() const;

signals:
    void packet_ready();

private:
    int capacity_ms_ = 200;           // 200ms 抖动缓冲（语义深度）
    std::deque<RTPPacket> buffer_;    // 排序缓冲（队首 = seq 最小）
    uint16_t last_seqno_ = 0;         // 最近 seq，用于丢包检测
    bool     has_last_seqno_ = false; // 是否已初始化基线
    uint32_t lost_packets_ = 0;       // 累计丢失包数
    uint32_t total_packets_ = 0;      // 累计接收包数
};

/**
 * @brief 带宽估计器 — 基于丢包率和延迟的自适应调整
 *
 * 弱网自适应：结合【丢包率 + RTT】两项指标，估算当前可用带宽与
 * 网络质量，通过信号通知编码器降/升码率、通知 UI 显示质量。
 *
 * 平滑机制：上报的丢包率/RTT 先经过 EMA 指数平滑（避免单次抖动
 * 导致码率剧烈振荡），再综合计算。
 */
class BandwidthEstimator : public QObject {
    Q_OBJECT
public:
    explicit BandwidthEstimator(QObject* parent = nullptr);

    void report_loss(double loss_rate);   // 上报丢包率（EMA 平滑）
    void report_rtt(double rtt_ms);       // 上报 RTT（EMA 平滑）
    void report_bitrate(uint32_t bitrate_kbps);

    uint32_t estimated_bandwidth() const { return estimated_kbps_; }
    int32_t  network_quality() const { return quality_; } // 0-5, 5=最优

signals:
    void bandwidth_changed(uint32_t new_bitrate_kbps);   // 触发编码器调码率
    void quality_changed(int32_t quality);               // 触发 UI 显示网络状态

private:
    void recalculate();   // 综合丢包+RTT 重算带宽/质量

    uint32_t estimated_kbps_ = 2500;   // 当前估算带宽（kbps）
    int32_t  quality_ = 5;             // 网络质量 0-5
    double   current_loss_ = 0.0;      // 平滑后的丢包率
    double   current_rtt_ = 0.0;       // 平滑后的 RTT

    // 平滑参数（EMA 系数，越大对新采样越敏感）
    static constexpr double kLossAlpha = 0.3;
    static constexpr double kRttAlpha = 0.2;

    // 丢包率阈值（按从优到差排列）
    static constexpr double kLossExcellent = 0.01;  // 优秀
    static constexpr double kLossGood = 0.03;       // 良好
    static constexpr double kLossModerate = 0.05;   // 中等
    static constexpr double kLossPoor = 0.10;       // 差

    // RTT 阈值（毫秒）
    static constexpr double kRttExcellent = 50.0;
    static constexpr double kRttGood = 100.0;
    static constexpr double kRttModerate = 200.0;
    static constexpr double kRttPoor = 400.0;
};
