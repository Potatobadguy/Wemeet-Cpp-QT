#include "rtp_session.h"
#include <QtEndian>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <chrono>

// ── RtpSession ──────────────────────────────────────────────

/**
 * RTP 会话构造：
 *  - 创建 UDP socket（媒体走 UDP，允许少量丢包换取低延迟）；
 *  - 启动码率统计计时器；
 *  - 把 socket 的 readyRead 信号连到 on_ready_read —— 数据到达时
 *    Qt 事件循环自动触发接收，无需忙等。
 */
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

/**
 * @brief 发送一帧视频
 *
 * 视频帧往往大于 MTU（如 640x480 JPEG 可能几 KB~几十 KB），因此
 * 必须【分片】：按 1400 字节切成多个 RTP 包，每包独立序列号递增。
 *
 * 分片语义：
 *  - 同一帧的所有分片共享同一个 timestamp（video_ts_）；
 *  - 每分片 sequence 递增，接收端靠 seq 判断是否完整、有无丢包；
 *  - 只有该帧【最后一片】且是【关键帧】时 marker=1，标记"一帧结束"
 *    （接收端可据此重组/刷新画面）。
 *
 * 时间戳推进：30fps → 每帧推进 3000 个时钟单位（90000Hz/30fps）。
 * 90000Hz 是 RTP 视频的标准采样率，接收端据此还原播放节奏。
 */
void RtpSession::send_video_frame(const QByteArray& frame_data, bool is_keyframe,
                                   const QHostAddress& dest, uint16_t dest_port) {
    // 大帧分片发送 (MTU ~1400) —— 低于常见 MTU(1500) 留出 IP/UDP/RTP 头空间
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
        pkt.set_marker(is_keyframe && (i == total_packets - 1));  // 最后一片标记
        pkt.set_sequence(video_seq_++);   // 每分片 seq 递增
        pkt.set_timestamp(video_ts_);     // 同一帧共享时间戳
        pkt.set_ssrc(ssrc_);              // 本会话固定 SSRC
        pkt.payload = frame_data.mid(offset, chunk_size);

        send_packet(pkt, dest, dest_port);
        offset += chunk_size;
    }

    // 帧率约 30fps → 每帧 3000 时钟增量（90000Hz / 30fps）
    video_ts_ += 3000;

    // 更新码率统计
    bytes_since_last_bitrate_ += frame_data.size();
    stats_.packets_sent += total_packets;

    // 前向纠错：用"本帧首分片"与"上一帧末分片"异或生成冗余包
    if (fec_enabled_ && total_packets > 0) {
        QByteArray first_payload = frame_data.mid(0, std::min<int>(kMTU, frame_size));
        send_video_fec(first_payload, total_packets, dest, dest_port);
    }
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

/**
 * @brief UDP socket 可读回调：接收媒体包
 *
 * 用 while 循环把当前积压的所有数据报一次取空（UDP 一个数据报 = 一个
 * RTP/RTCP 包，无粘包问题）。对每个包：
 *  - 校验长度 >= RTP 头（12 字节）；
 *  - 反序列化为 RTPPacket；
 *  - 按 payload type 区分：>=200 是 RTCP 控制包（简化跳过），
 *    <200 是媒体包 → emit packet_received 交给上层（MediaEngine），
 *    上层会送入 JitterBuffer 排序缓冲。
 */
void RtpSession::on_ready_read() {
    while (socket_->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_->pendingDatagramSize()));
        QHostAddress src_addr;
        uint16_t src_port;

        socket_->readDatagram(datagram.data(), datagram.size(), &src_addr, &src_port);

        if (datagram.size() < static_cast<int>(sizeof(RTPHeader))) continue;

        RTPPacket pkt = RTPPacket::deserialize(datagram);
        // deserialize 内置 validate_rtp_packet 校验（#17）：非法包 payload 为空 → 丢弃
        if (pkt.payload.isEmpty()) continue;
        stats_.packets_received++;
        stats_.bytes_received += datagram.size();

        // RTCP 包（SR=200 / RR=201 / SDES=202 / BYE=203）
        if (pkt.payload_type() >= 200) {
            parse_rtcp_packet(datagram);
            continue;
        }

        // FEC 冗余包：不进入媒体流，解析后存入 FEC 缓存
        if (pkt.payload_type() == pt_fec_) {
            parse_fec_packet(pkt);
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

// ── FEC 发送 ────────────────────────────────────────────────
void RtpSession::send_video_fec(const QByteArray& first_payload, int frame_packets,
                                const QHostAddress& dest, uint16_t dest_port) {
    if (last_video_packet_.isEmpty()) {
        // 首个视频包：缓存本帧首分片，暂不生成冗余
        last_video_packet_ = first_payload;
        return;
    }

    // 本帧首分片 = d1，上一帧首分片 = d2，XOR 得到冗余数据
    QByteArray xor_data = generate_fec(first_payload, last_video_packet_);
    last_video_packet_ = first_payload;   // 缓存本帧，供下一帧使用

    // 冗余包负载: [2B 保护起始seq][2B 保护结束seq][XOR data]
    uint16_t start_seq = static_cast<uint16_t>(video_seq_ - frame_packets);
    uint16_t end_seq   = static_cast<uint16_t>(video_seq_ - 1);

    RTPPacket pkt;
    std::memset(&pkt.header, 0, sizeof(RTPHeader));
    pkt.header.cc_version = (2 << 6);
    pkt.set_payload_type(pt_fec_);
    pkt.set_sequence(fec_seq_++);
    pkt.set_timestamp(video_ts_);
    pkt.set_ssrc(ssrc_);

    QByteArray body;
    body.append(static_cast<char>((start_seq >> 8) & 0xFF));
    body.append(static_cast<char>(start_seq & 0xFF));
    body.append(static_cast<char>((end_seq >> 8) & 0xFF));
    body.append(static_cast<char>(end_seq & 0xFF));
    body.append(xor_data);
    pkt.payload = body;

    send_packet(pkt, dest, dest_port);
}

// ── FEC 接收解析 ────────────────────────────────────────────
void RtpSession::parse_fec_packet(const RTPPacket& pkt) {
    // 负载格式: [2B 保护起始seq][2B 保护结束seq][XOR data]
    if (pkt.payload.size() < 4) return;

    uint16_t start_seq = (static_cast<uint16_t>(pkt.payload[0]) << 8) |
                          static_cast<uint16_t>(pkt.payload[1]);
    uint16_t end_seq   = (static_cast<uint16_t>(pkt.payload[2]) << 8) |
                          static_cast<uint16_t>(pkt.payload[3]);
    if (start_seq > end_seq) return;

    // 只保留最近 32 组，防止缓存无限增长
    if (fec_cache_.size() > 32) {
        fec_cache_.erase(fec_cache_.begin());
    }

    FecRecord rec;
    rec.start_seq = start_seq;
    rec.end_seq   = end_seq;
    rec.xor_data  = pkt.payload.mid(4);
    fec_cache_[start_seq] = std::move(rec);
}

bool RtpSession::recover_fec(uint16_t lost_seq, QByteArray& data) {
    // 找到覆盖 lost_seq 的 FEC 记录
    for (auto& [start, rec] : fec_cache_) {
        if (lost_seq >= rec.start_seq && lost_seq <= rec.end_seq) {
            // XOR 数据即为该组的冗余，这里还原逻辑依赖上层补齐组内另一包。
            // 简化方案：将冗余数据直接作为还原结果（对两包 XOR 场景，
            // 若上层已缓存组内另一包，可再 XOR；此处返回原始冗余供上层判断）。
            data = rec.xor_data;
            return true;
        }
    }
    return false;
}

// ── RTCP 解析 ───────────────────────────────────────────────
void RtpSession::parse_rtcp_packet(const QByteArray& datagram) {
    // 解析 Sender Report (PT=200) 中的发送时间戳与 NTP，用于估算 RTT。
    // 帧结构: [8B 头][4B SSRC][8B NTP][4B RTP ts][4B pkt][4B octet]
    if (datagram.size() < 28) return;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(datagram.constData());
    // p[0]: version/PT 的低字节; PT 在 p[1]（见 RTP 头字段布局）
    uint8_t pt = p[1] & 0x7F;
    if (pt != 200) return;  // 仅处理 SR

    // NTP 时间戳（64bit，大端）位于偏移 8..16
    uint64_t ntp_hi = (static_cast<uint64_t>(p[8]) << 24) |
                      (static_cast<uint64_t>(p[9]) << 16) |
                      (static_cast<uint64_t>(p[10]) << 8) |
                      static_cast<uint64_t>(p[11]);
    uint64_t ntp_lo = (static_cast<uint64_t>(p[12]) << 24) |
                      (static_cast<uint64_t>(p[13]) << 16) |
                      (static_cast<uint64_t>(p[14]) << 8) |
                      static_cast<uint64_t>(p[15]);

    // 换算当前本地毫秒：NTP 秒数 - 2208988800 = Unix 秒
    double ntp_ms = (static_cast<double>(ntp_hi) - 2208988800.0) * 1000.0 +
                    (static_cast<double>(ntp_lo) * 1000.0) / 0x100000000ULL;
    double now_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    // 若 SR 时间晚于本地时钟（时钟不同步），无法可靠计算，跳过
    if (ntp_ms > now_ms) return;

    // 粗略 RTT 估计：发送端 SR 与本地接收时刻的差（单向时间近似，
    // 更准确的 RTT 需回 RR 并等待对端回执，这里做单程估算）
    double est_rtt = now_ms - ntp_ms;
    if (est_rtt > 0 && est_rtt < 10000) {
        stats_.rtt_ms = est_rtt;
    }
}

// ── JitterBuffer ────────────────────────────────────────────

JitterBuffer::JitterBuffer(QObject* parent) : QObject(parent) {}

/**
 * @brief 推入一个 RTP 包（乱序到达时按 seq 排序插入）
 *
 * ─────────────────────────────────────────────────────────────
 *  抖动缓冲（Jitter Buffer）如何平滑 200ms 抖动？
 * ─────────────────────────────────────────────────────────────
 *  问题：网络抖动导致 RTP 包到达时间不稳定——同一帧的分片可能
 *  早到/晚到、甚至乱序。若收到就立即播放，就会出现卡顿、花屏。
 *
 *  解法：**缓冲 + 排序 + 按时间戳播放**。
 *   - 缓冲：包先不播放，在 buffer_ 里暂存一段时间（capacity_ms_=200ms），
 *     让"早到的包等一等，晚到的包赶上来"；
 *   - 排序：push_packet 按 seq 升序插入（见下方插入逻辑），
 *     pop_packet 永远取 seq 最小的包，从而把乱序纠正为有序；
 *   - 播放节奏：由上层定时器（约按 200ms 缓冲深度）触发 pop，
 *     稳定地按时间戳间隔送给播放器，抹平到达时间的忽早忽晚。
 *
 *  关键点：
 *   - "200ms" 指的是【缓冲深度/容量】，即播放器比真实到达滞后最多
 *     200ms 的"人为延迟"，用于吸收 200ms 以内的抖动；
 *   - 代价：延迟增加 200ms（延迟换平滑），所以 JitterBuffer 只用于
 *     接收/播放端，发送端不加；
 *   - 若实际抖动超过 200ms，仍会丢包——这就是 capacity 上限的意义，
 *     也是丢包检测（下方）存在的原因。
 * ─────────────────────────────────────────────────────────────
 *  丢包检测：seq 出现空洞（seq > last_seqno_+1）说明中间有包丢失，
 *  累计 lost_packets_ 供 loss_rate() 计算丢包率，喂给带宽估计器。
 */
void JitterBuffer::push_packet(const RTPPacket& packet) {
    uint16_t seq = packet.sequence();
    total_packets_++;

    // 第一个包：初始化基线，直接入队
    if (!has_last_seqno_) {
        has_last_seqno_ = true;
        last_seqno_ = seq;
        buffer_.push_back(packet);
        emit packet_ready();
        return;
    }

    // 检测丢包：seq 跳变说明有包丢失
    if (seq > last_seqno_ + 1) {
        lost_packets_ += (seq - last_seqno_ - 1);
    }
    last_seqno_ = seq;

    // 按序列号排序插入（deque 的 insert，复杂度 O(n)，包量小可接受）
    auto it = buffer_.begin();
    while (it != buffer_.end() && it->sequence() < seq) {
        ++it;
    }
    buffer_.insert(it, packet);

    // 限制缓冲区大小：上限 500 包，防止异常/大抖动时内存膨胀；
    // 弹出最老的（front），保持缓冲不超限。
    if (buffer_.size() > 500) {
        buffer_.pop_front();
    }
}

/**
 * @brief 弹出 seq 最小的包（排序缓冲，永远取队首）
 * @return false 表示缓冲区为空，无包可弹
 */
bool JitterBuffer::pop_packet(RTPPacket& packet) {
    if (buffer_.empty()) return false;
    packet = buffer_.front();   // front 是 seq 最小者（因插入已排序）
    buffer_.pop_front();
    return true;
}

/**
 * @brief 清空缓冲并重置所有状态（切换流/断线时调用）
 */
void JitterBuffer::clear() {
    buffer_.clear();
    has_last_seqno_ = false;
    last_seqno_ = 0;
    lost_packets_ = 0;
    total_packets_ = 0;
}

/**
 * @brief 计算丢包率（供 BandwidthEstimator 做弱网自适应）
 */
double JitterBuffer::loss_rate() const {
    if (total_packets_ == 0) return 0.0;
    return static_cast<double>(lost_packets_) / total_packets_;
}

// ── BandwidthEstimator ──────────────────────────────────────

BandwidthEstimator::BandwidthEstimator(QObject* parent) : QObject(parent) {}

/**
 * @brief 上报丢包率（EMA 指数平滑后重算）
 *
 * 用指数移动平均（kLossAlpha=0.3）平滑：`新值 = 0.7*旧值 + 0.3*采样`，
 * 避免单次抖动剧烈跳变，让带宽调整更稳定。
 */
void BandwidthEstimator::report_loss(double loss_rate) {
    current_loss_ = (1.0 - kLossAlpha) * current_loss_ + kLossAlpha * loss_rate;
    recalculate();
}

/**
 * @brief 上报 RTT（EMA 平滑，kRttAlpha=0.2）
 */
void BandwidthEstimator::report_rtt(double rtt_ms) {
    current_rtt_ = (1.0 - kRttAlpha) * current_rtt_ + kRttAlpha * rtt_ms;
    recalculate();
}

void BandwidthEstimator::report_bitrate(uint32_t bitrate_kbps) {
    recalculate();
}

/**
 * @brief 综合丢包率 + RTT，重新估算带宽与网络质量
 *
 * 规则：
 *  - 丢包率越差 → 质量档越低、带宽上限越低（min 逐步收紧）；
 *  - RTT 越大 → 进一步打折带宽（*0.85 / *0.7 / *0.5）；
 *  - 二者取【双重约束】：先按丢包设上限，再按 RTT 降档/打折，
 *    保证带宽建议永远 ≤ 当前允许值；
 *  - 收敛范围 [200kbps, 5000kbps]，避免过小/过大。
 *
 * 质量分 0-5（5 最优），变化时通过信号通知 UI 显示网络状态。
 */
void BandwidthEstimator::recalculate() {
    int32_t new_quality = 5;
    uint32_t new_bw = estimated_kbps_;

    // 基于丢包率（先设上限）
    if (current_loss_ > kLossPoor) {           // >10% 网络差
        new_quality = 1;
        new_bw = std::min(new_bw, 500u);
    } else if (current_loss_ > kLossModerate) { // >5%
        new_quality = 2;
        new_bw = std::min(new_bw, 1000u);
    } else if (current_loss_ > kLossGood) {     // >3%
        new_quality = 3;
        new_bw = std::min(new_bw, 1500u);
    } else if (current_loss_ > kLossExcellent) { // >1%
        new_quality = 4;
        new_bw = std::min(new_bw, 2000u);
    } else {                                    // 优秀
        new_quality = 5;
        new_bw = 2500u;
    }

    // 基于 RTT（在丢包基础上进一步降档/打折）
    if (current_rtt_ > kRttPoor) {              // >400ms
        new_quality = std::min(new_quality, 1);
        new_bw = new_bw * 0.5;
    } else if (current_rtt_ > kRttModerate) {   // >200ms
        new_quality = std::min(new_quality, 2);
        new_bw = new_bw * 0.7;
    } else if (current_rtt_ > kRttGood) {       // >100ms
        new_quality = std::min(new_quality, 3);
        new_bw = new_bw * 0.85;
    }

    // 确保最小带宽（200kbps 保底）与最大带宽（5000kbps 封顶）
    new_bw = std::max(new_bw, 200u);
    new_bw = std::min(new_bw, 5000u);

    // 质量变化 → 通知 UI
    if (new_quality != quality_) {
        quality_ = new_quality;
        emit quality_changed(quality_);
    }

    // 带宽变化 → 通知编码器调整码率
    if (new_bw != estimated_kbps_) {
        estimated_kbps_ = new_bw;
        emit bandwidth_changed(estimated_kbps_);
    }
}
