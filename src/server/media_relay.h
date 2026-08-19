#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <random>
#include <arpa/inet.h>   // htons, htonl, ntohs, ntohl, sockaddr_in
#include <netinet/in.h>  // INET_ADDRSTRLEN
#include "rtp_header.h"  // common/rtp_header.h — RTPHeader 唯一定义（#9）

namespace wemeet {

// RTPHeader 唯一定义在 src/common/rtp_header.h（#9），此处 using 保持兼容
using wemeet::RTPHeader;

// RTCPReportBlock 位域结构已删除（#11）：位域不应用于网络协议解析
// （字节序/位序不可移植），RTCP 解析统一走显式字节读取（media_relay.cpp）。

/**
 * @brief 参与者 RTP 流信息
 *
 * 一个参与者可以有多路流（video / audio / screen），每路一个
 * RTPStreamInfo，但共享同一个 SSRC（见 register_participant 注释）。
 */
struct RTPStreamInfo {
    uint32_t ssrc = 0;
    std::string media_type;   // "video" / "audio" / "screen"
    std::string client_host;  // 客户端地址字符串（日志/调试用）
    uint16_t client_port = 0;
    std::string relay_host;   // 中继地址（返回给客户端，告知往哪发）
    uint16_t relay_port = 0;

    // 预解析的客户端目标地址（#5）：注册时 inet_pton 一次，
    // 转发热路径直接 sendto，不再逐包解析 IP 字符串
    struct sockaddr_in client_addr_in {};

    // 统计（原子：被多个 relay 线程读写）
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};   // 本流从客户端收到的字节数（relay 转发路径累计）
    std::atomic<double> packet_loss_rate{0.0};
    std::atomic<double> rtt_ms{0.0};

    // 连接状态
    std::atomic<bool> active{true};              // 流是否活跃（静音/关摄像头时 false）
    std::atomic<uint64_t> last_active_time{0};   // 最后活跃时刻（用于超时判断）
};

/**
 * @brief 媒体中继参与者
 *
 * 一名在线用户在中继侧的状态，内部用 map 管理其多路媒体流。
 */
struct RelayParticipant {
    uint64_t user_id = 0;
    std::string nickname;
    std::unordered_map<std::string, std::shared_ptr<RTPStreamInfo>> streams; // media_type → 流
    bool audio_on = true;
    bool video_on = true;
    bool screen_sharing = false;
    uint64_t last_heartbeat{0};   // 心跳时间戳（用于断线清理）
};

/**
 * @brief 媒体中继房间
 *
 * SFU 按房间隔离转发：只有同房间的参与者才互通音视频。
 * 每房间一把锁，配合全局 rooms_mutex_ 形成两级锁（media_relay.cpp）。
 */
struct RelayRoom {
    std::string room_id;
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<RelayParticipant>> participants;

    /**
     * @brief 房间内广播（发给除发送者外的活跃流）
     * @param sender_id 发送者用户 ID（跳过自己，不回环）
     * @param media_type 媒体类型，只转发对应类型活跃的流
     * @param send_func 实际发送回调（由 MediaRelay 注入）
     */
    void broadcast_data(uint64_t sender_id, const std::string& media_type,
                        const uint8_t* data, size_t len,
                        const std::function<void(uint64_t, const uint8_t*, size_t)>& send_func);
};

/**
 * @brief SSRC 路由索引项（#3）— ssrc_index_ 的值类型
 *
 * 转发/RTCP 热路径通过 ssrc → SsrcRoute 一次哈希命中拿到房间、
 * 发送者与流指针，不再遍历 rooms_。持有 shared_ptr 保证房间/流
 * 在转发期间不会被析构（绕开 rooms_mutex_）。
 */
struct SsrcRoute {
    std::shared_ptr<RelayRoom>      room;        // 持有房间，绕开 rooms_mutex_
    uint64_t                        sender_id = 0;
    std::string                     media_type;
    std::shared_ptr<RTPStreamInfo>  stream;      // 直接更新统计，无需再遍历
};

/**
 * @brief 服务器端媒体中继 — SFU (Selective Forwarding Unit)
 *
 * ─────────────────────────────────────────────────────────────
 *  SFU 架构说明
 * ─────────────────────────────────────────────────────────────
 *  - SFU = 选择性转发单元：客户端不直连，所有音视频 RTP 先发给中继，
 *    中继按房间选择性转发给其他参与者。
 *  - 每个中继线程独立一个 UDP 端口，按 media_type 把不同流分配到
 *    不同端口（audio→0号, video→1号），实现并行收包。
 *
 *  锁序约定（防死锁，全局唯一顺序）：
 *      rooms_mutex_ → room->mutex → ssrc_index_mutex_
 *  转发/RTCP 热路径只取 "ssrc_index_ 共享锁 → room->mutex"，
 *  不触碰 rooms_mutex_，禁止反向取锁。
 *
 *  负责：UDP 媒体包的接收与转发、带宽自适应建议、统计收集
 */
class MediaRelay {
public:
    explicit MediaRelay(const std::string& bind_ip = "0.0.0.0",
                        uint16_t base_port = 10000);
    ~MediaRelay();

    MediaRelay(const MediaRelay&) = delete;
    MediaRelay& operator=(const MediaRelay&) = delete;

    // ── 生命周期 ─────────────────────────────────────────
    /**
     * @brief 启动 N 个中继线程
     * @param worker_threads 线程数，每个线程监听一个端口 (base_port+i)
     */
    bool start(int worker_threads = 2);
    void stop();

    // ── 参与者管理 ───────────────────────────────────────
    /**
     * @brief 注册参与者的一路媒体流（信令层调用）
     *
     * 核心职责：
     *  - 为该用户分配/复用 SSRC（同一用户多流共享一个 SSRC）；
     *  - 确定中继端口（按 media_type 路由）；
     *  - inet_pton 预解析客户端地址存入 RTPStreamInfo::client_addr_in；
     *  - 把 (ssrc → SsrcRoute) 插入 ssrc_index_，供热路径 O(1) 反查。
     *
     * @param out_ssrc      [out] 分配的 SSRC（回传给客户端）
     * @param out_relay_host/out_relay_port [out] 中继地址（告知客户端往哪发）
     */
    bool register_participant(uint64_t user_id, const std::string& room_id,
                              const std::string& nickname,
                              const std::string& client_host, uint16_t client_port,
                              const std::string& media_type,
                              uint32_t& out_ssrc,
                              std::string& out_relay_host,
                              uint16_t& out_relay_port);

    /**
     * @brief 移除参与者（离开会议/断线清理，幂等）。
     *        同步从 ssrc_index_ 批量删除该用户全部 SSRC。
     */
    void unregister_participant(uint64_t user_id, const std::string& room_id);

    /**
     * @brief 更新音视频开关状态（静音/关摄像头时把对应流置为不活跃，
     *        转发逻辑据此跳过该流）
     */
    void update_media_status(uint64_t user_id, const std::string& room_id,
                             bool audio_on, bool video_on);

    // ── 带宽控制 ─────────────────────────────────────────
    /**
     * @brief 上报客户端统计（丢包/RTT/抖动/码率），驱动自适应。
     *        锁内仅更新统计与计算建议，带宽提示在释放全部锁后发送（#1）。
     */
    void report_stats(uint64_t user_id, const std::string& room_id,
                      double packet_loss, double rtt, double jitter,
                      uint32_t bitrate, int32_t quality);

    /**
     * @brief 依据丢包率建议码率（分级 300~2500 kbps），弱网自适应核心。
     *        公共版本：自加锁后委托 suggest_bitrate_unlocked()。
     */
    uint32_t suggest_bitrate(uint64_t user_id, const std::string& room_id);

    // ── 查询 ─────────────────────────────────────────────
    /**
     * @brief 获取房间参与者快照（返回值副本，安全）。
     *        注意：非热路径使用（信令层查询），转发循环不得调用。
     */
    std::vector<RelayParticipant> get_room_participants(const std::string& room_id);

    /**
     * @brief 分配一个随机 SSRC（mt19937 + ssrc_index_ 冲突检测重试，#12）。
     *        公共版本：自加锁后委托 allocate_ssrc_locked()。
     */
    uint32_t allocate_ssrc();

    /**
     * @brief 媒体包回调（信令集成用，当前主要记录日志）
     */
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

    /**
     * @brief 建议码率的 _unlocked 版本（#1）
     *
     * 命名约定：_unlocked 后缀表示"假定调用者已持有 room->mutex"，
     * 仅在持锁上下文（如 report_stats）调用，自身不再取任何锁，
     * 从根本上消除 report_stats → suggest_bitrate 的递归死锁。
     */
    uint32_t suggest_bitrate_unlocked(const std::shared_ptr<RelayRoom>& room,
                                      uint64_t user_id);

    /**
     * @brief SSRC 分配的 _unlocked 版本：假定已持有 ssrc_index_mutex_
     *        独占锁。mt19937 随机生成 + 查索引冲突则重试（#12）。
     */
    uint32_t allocate_ssrc_locked();

    /**
     * @brief SSRC → 路由 O(1) 查找（#3）。内部取 ssrc_index_mutex_ 共享锁。
     * @return true 时 out_route 填入完整路由（shared_ptr 拷贝，锁外可用）
     */
    bool lookup_ssrc(uint32_t ssrc, SsrcRoute& out_route);

    /**
     * @brief 线程安全地址格式化（#2）：inet_ntop + 调用方栈缓冲区，
     *        替代非线程安全的 inet_ntoa。
     */
    static const char* format_addr(const struct sockaddr_in& addr, char* buf);

    std::string bind_ip_;
    uint16_t base_port_;
    std::atomic<bool> running_{false};

    // 多线程接收
    int worker_threads_ = 2;
    std::vector<std::thread> relay_threads_;   // 每个线程一个接收循环
    std::vector<int> relay_sockets_;           // 每个线程一个 UDP socket
    std::mutex sockets_mutex_;                 // relay_sockets_ 的线程安全保护（#13）

    // 房间管理（两级锁：全局 + 每房间）
    std::mutex rooms_mutex_;
    std::unordered_map<std::string, std::shared_ptr<RelayRoom>> rooms_;

    // SSRC → 路由哈希索引（#3）：注册/注销独占写，转发共享读
    std::shared_mutex ssrc_index_mutex_;
    std::unordered_map<uint32_t, SsrcRoute> ssrc_index_;

    // SSRC 随机分配器（#12）：仅在持有 ssrc_index_mutex_ 独占锁时使用
    std::mt19937 rng_{std::random_device{}()};

    // 回调
    PacketCallback packet_cb_;

    // 统计
    std::atomic<uint64_t> total_packets_forwarded_{0};   // 累计转发包数
    std::atomic<uint64_t> invalid_packets_{0};           // 非法包计数（#17）
    std::atomic<uint64_t> fwd_seq_{0};                   // 转发摘要计数（#18 成员化）
    std::atomic<uint64_t> unknown_ssrc_seq_{0};          // 未知 SSRC 计数（#18 成员化）
};

} // namespace wemeet
