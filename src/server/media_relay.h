#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <arpa/inet.h>   // htons, htonl, ntohs, ntohl

namespace wemeet {

/**
 * @brief RTP 媒体包头部（标准 RTPv2 格式 12 字节）
 */
#pragma pack(push, 1)
struct RTPHeader {
    uint8_t  cc_version;       // [0:3] CSRC count, [4:7] version=2
    uint8_t  pt_marker;        // [0:6] payload type, [7] marker
    uint16_t sequence_number;  // 序列号（网络字节序）
    uint32_t timestamp;        // 时间戳（网络字节序）
    uint32_t ssrc;             // 同步源标识（网络字节序）

    void set_version() { cc_version = (cc_version & 0x0F) | (2 << 6); }
    void set_marker(bool m) { pt_marker = (pt_marker & 0x7F) | (m ? 0x80 : 0); }
    void set_payload_type(uint8_t pt) { pt_marker = (pt_marker & 0x80) | (pt & 0x7F); }
    void set_sequence(uint16_t seq) { sequence_number = htons(seq); }
    void set_timestamp(uint32_t ts) { timestamp = htonl(ts); }
    void set_ssrc(uint32_t id) { ssrc = htonl(id); }

    uint8_t  payload_type() const { return pt_marker & 0x7F; }
    bool     marker() const { return (pt_marker & 0x80) != 0; }
    uint16_t sequence() const { return ntohs(sequence_number); }
    uint32_t timestamp_val() const { return ntohl(timestamp); }
    uint32_t ssrc_val() const { return ntohl(ssrc); }

    static constexpr size_t kHeaderSize = 12;
};
#pragma pack(pop)

/**
 * @brief RTCP 接收报告块
 */
#pragma pack(push, 1)
struct RTCPReportBlock {
    uint32_t ssrc_source;       // 源 SSRC
    uint32_t fraction_lost : 8; // 丢包率
    uint32_t cumulative_lost : 24; // 累计丢包数
    uint32_t highest_seq;      // 最高收到的序列号
    uint32_t interarrival_jitter; // 到达间隔抖动
    uint32_t last_sr_timestamp; // 上次 SR 时间戳
    uint32_t delay_since_last_sr; // 上次 SR 后的延迟
};
#pragma pack(pop)

/**
 * @brief 参与者 RTP 流信息
 */
struct RTPStreamInfo {
    uint32_t ssrc = 0;
    std::string media_type;   // "video" / "audio" / "screen"
    std::string client_host;
    uint16_t client_port = 0;
    std::string relay_host;
    uint16_t relay_port = 0;

    // 统计
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<double> packet_loss_rate{0.0};
    std::atomic<double> rtt_ms{0.0};

    // 连接状态
    std::atomic<bool> active{true};
    std::atomic<uint64_t> last_active_time{0};
};

/**
 * @brief 媒体中继参与者
 */
struct RelayParticipant {
    uint64_t user_id = 0;
    std::string nickname;
    std::unordered_map<std::string, std::shared_ptr<RTPStreamInfo>> streams;
    bool audio_on = true;
    bool video_on = true;
    bool screen_sharing = false;
    uint64_t last_heartbeat{0};
};

/**
 * @brief 媒体中继房间
 */
struct RelayRoom {
    std::string room_id;
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<RelayParticipant>> participants;

    void broadcast_data(uint64_t sender_id, const std::string& media_type,
                        const uint8_t* data, size_t len,
                        const std::function<void(uint64_t, const uint8_t*, size_t)>& send_func);
};

/**
 * @brief 服务器端媒体中继 — SFU (Selective Forwarding Unit)
 *
 * 负责：UDP 媒体包的接收与转发、带宽自适应建议、统计收集
 */
class MediaRelay {
public:
    explicit MediaRelay(const std::string& bind_ip = "0.0.0.0",
                        uint16_t base_port = 10000);
    ~MediaRelay();

    MediaRelay(const MediaRelay&) = delete;
    MediaRelay& operator=(const MediaRelay&) = delete;

    // ── 生命周期 ─────────────────────────────────────────
    bool start(int worker_threads = 2);
    void stop();

    // ── 参与者管理 ───────────────────────────────────────
    bool register_participant(uint64_t user_id, const std::string& room_id,
                              const std::string& nickname,
                              const std::string& client_host, uint16_t client_port,
                              const std::string& media_type,
                              uint32_t& out_ssrc,
                              std::string& out_relay_host,
                              uint16_t& out_relay_port);

    void unregister_participant(uint64_t user_id, const std::string& room_id);
    void update_media_status(uint64_t user_id, const std::string& room_id,
                             bool audio_on, bool video_on);

    // ── 带宽控制 ─────────────────────────────────────────
    void report_stats(uint64_t user_id, const std::string& room_id,
                      double packet_loss, double rtt, double jitter,
                      uint32_t bitrate, int32_t quality);
    uint32_t suggest_bitrate(uint64_t user_id, const std::string& room_id);

    // ── 查询 ─────────────────────────────────────────────
    std::vector<RelayParticipant> get_room_participants(const std::string& room_id);
    uint32_t allocate_ssrc();

    using PacketCallback = std::function<void(uint64_t from_user_id,
                                              const std::string& media_type,
                                              const uint8_t* data, size_t len)>;
    void set_packet_callback(PacketCallback cb) { packet_cb_ = std::move(cb); }

private:
    void relay_thread_func(int thread_id, uint16_t port);
    void handle_rtcp_packet(const uint8_t* data, size_t len,
                            const std::string& client_addr, uint16_t client_port);
    void send_bandwidth_hint(const std::shared_ptr<RelayParticipant>& participant,
                             uint32_t suggested_bitrate);

    std::string bind_ip_;
    uint16_t base_port_;
    std::atomic<bool> running_{false};

    // 多线程接收
    int worker_threads_ = 2;
    std::vector<std::thread> relay_threads_;
    std::vector<int> relay_sockets_;  // 每个线程一个 UDP socket

    // 房间管理
    std::mutex rooms_mutex_;
    std::unordered_map<std::string, std::shared_ptr<RelayRoom>> rooms_;

    // SSRC 分配
    std::atomic<uint32_t> next_ssrc_{1000};

    // 回调
    PacketCallback packet_cb_;

    // 统计
    std::atomic<uint64_t> total_packets_forwarded_{0};
};

} // namespace wemeet
