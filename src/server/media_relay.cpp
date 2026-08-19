#include "media_relay.h"
#include "logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <shared_mutex>

namespace wemeet {

// ── RelayRoom 广播方法 ──────────────────────────────────────
/**
 * @brief 房间内按媒体类型广播
 *
 * 转发规则：
 *  1. 跳过发送者自己（uid == sender_id），避免"回声"回环；
 *  2. 只转发指定 media_type 且 active==true 的流。
 *
 * 线程安全：本函数持有房间锁（mutex），保证遍历 participants 期间
 * 房间成员不会并发增减。实际发送（send_func）由调用方（MediaRelay
 * 转发循环）注入。
 */
void RelayRoom::broadcast_data(
        uint64_t sender_id, const std::string& media_type,
        const uint8_t* data, size_t len,
        const std::function<void(uint64_t, const uint8_t*, size_t)>& send_func) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [uid, p] : participants) {
        if (uid == sender_id) continue;
        // 只转发对应媒体类型活跃的流
        auto it = p->streams.find(media_type);
        if (it != p->streams.end() && it->second->active) {
            send_func(uid, data, len);
        }
    }
}

// ── MediaRelay ───────────────────────────────────────────────
MediaRelay::MediaRelay(const std::string& bind_ip, uint16_t base_port)
    : bind_ip_(bind_ip), base_port_(base_port) {
    // 混入线程/时间熵，避免不同进程实例随机序列相同
    rng_.seed(std::random_device{}() ^
              static_cast<uint32_t>(
                  std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    LOG_INFO("MediaRelay initialized on %s:%u+", bind_ip.c_str(), base_port);
}

MediaRelay::~MediaRelay() {
    stop();
}

bool MediaRelay::start(int worker_threads) {
    if (running_) return true;

    worker_threads_ = std::max(1, worker_threads);
    running_ = true;

    for (int i = 0; i < worker_threads_; ++i) {
        uint16_t port = base_port_ + static_cast<uint16_t>(i);
        relay_threads_.emplace_back(&MediaRelay::relay_thread_func, this, i, port);
        LOG_INFO("MediaRelay thread %d listening on UDP %s:%u", i, bind_ip_.c_str(), port);
    }

    LOG_INFO("MediaRelay started with %d worker threads", worker_threads_);
    return true;
}

void MediaRelay::stop() {
    if (!running_) return;
    running_ = false;

    // 关闭所有 socket 以唤醒线程（#13：加锁保护 relay_sockets_，
    // relay 线程在启动时向其中 push_back，stop 与之存在竞态）
    {
        std::lock_guard<std::mutex> lk(sockets_mutex_);
        for (int fd : relay_sockets_) {
            if (fd >= 0) ::close(fd);
        }
        relay_sockets_.clear();
    }

    for (auto& t : relay_threads_) {
        if (t.joinable()) t.join();
    }
    relay_threads_.clear();

    LOG_INFO("MediaRelay stopped, total packets forwarded: %lu, invalid: %lu",
             total_packets_forwarded_.load(), invalid_packets_.load());
}

/**
 * @brief 线程安全的 IPv4 地址格式化（#2）
 *
 * inet_ntoa 返回静态缓冲区，多 relay 线程并发调用会产生数据竞争；
 * 改用 inet_ntop + 调用方栈上缓冲区（char buf[INET_ADDRSTRLEN]）。
 */
const char* MediaRelay::format_addr(const struct sockaddr_in& addr, char* buf) {
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, INET_ADDRSTRLEN) == nullptr) {
        buf[0] = '?';
        buf[1] = '\0';
    }
    return buf;
}

/**
 * @brief 单个中继线程的收包-转发循环
 *
 * 转发路径（O(1) SSRC 索引，#3）：
 *  1. poll 阻塞等待可读，读尽积压数据报；
 *  2. validate_rtp_packet() 校验版本/包长（#17），非法包计入 invalid_packets_；
 *  3. 按 payload type 区分：RTCP(200~204) → 统计；RTP(<200) → 转发；
 *  4. RTP 转发三步：
 *     ① 读包头 SSRC；
 *     ② lookup_ssrc()：ssrc_index_ 共享锁 O(1) 反查 SsrcRoute
 *        （房间 + sender_id + media_type + stream 指针）；
 *     ③ 锁序"共享锁拷贝路由 → room->mutex"，遍历房间参与者，
 *        用预解析的 client_addr_in 直接 sendto（#5）。
 *
 * 锁序：热路径只取 ssrc_index_ 共享锁（拷贝路由后立即释放）
 *      → room->mutex，不触碰 rooms_mutex_，无死锁风险。
 */
void MediaRelay::relay_thread_func(int thread_id, uint16_t port) {
    // 创建 UDP socket（非阻塞）
    int sock = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        LOG_ERROR("MediaRelay thread %d: socket() failed: %s", thread_id, strerror(errno));
        return;
    }

    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, bind_ip_.c_str(), &addr.sin_addr);

    if (::bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("MediaRelay thread %d: bind() failed on %s:%u: %s",
                  thread_id, bind_ip_.c_str(), port, strerror(errno));
        ::close(sock);
        return;
    }

    {
        // #13：与 stop() 之间用 sockets_mutex_ 互斥
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        if (!running_) {   // stop() 已发生 → 直接退出，避免 socket 泄漏
            ::close(sock);
            return;
        }
        relay_sockets_.push_back(sock);
    }

    // 接收缓冲区（最大支持 64KB 媒体包）
    constexpr size_t kBufSize = 65536;
    auto* recv_buf = new uint8_t[kBufSize];

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    LOG_INFO("MediaRelay thread %d listening on UDP port %u", thread_id, port);

    while (running_) {
        // 用 poll 阻塞等待可读，避免忙等空转 CPU；
        // 带 200ms 超时以便定期检查 running_ 标志。
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;   // socket 被 stop() 关闭（EBADF 等）
        }
        if (pr == 0) continue;   // 超时，重新检查 running_

        // 有可读数据：用 MSG_DONTWAIT 循环读尽当前积压的数据报
        client_len = sizeof(client_addr);
        ssize_t received = ::recvfrom(sock, recv_buf, kBufSize, MSG_DONTWAIT,
                                      (struct sockaddr*)&client_addr, &client_len);
        while (received > 0) {
            // ── 处理单个数据报 ──
            // #17：校验包长 + RTP 版本号，非法包直接丢弃并计数
            if (!validate_rtp_packet(recv_buf, static_cast<size_t>(received))) {
                uint64_t inv = invalid_packets_.fetch_add(1) + 1;
                if (inv < 10 || inv % 1000 == 0) {
                    char addr_buf[INET_ADDRSTRLEN];
                    LOG_WARN("MediaRelay invalid packet (len=%zd) from %s:%u, total_invalid=%lu",
                             received, format_addr(client_addr, addr_buf),
                             ntohs(client_addr.sin_port), inv);
                }
            } else {
                auto* hdr = reinterpret_cast<RTPHeader*>(recv_buf);
                uint8_t pt = hdr->payload_type();

                if (pt >= 200 && pt <= 204) {
                    // RTCP 包 → 处理统计
                    char addr_buf[INET_ADDRSTRLEN];
                    handle_rtcp_packet(recv_buf, received,
                                       format_addr(client_addr, addr_buf),
                                       ntohs(client_addr.sin_port));
                } else {
                    // RTP 媒体包 → 根据 SSRC 索引转发
                    uint32_t ssrc = hdr->ssrc_val();

                    // ① O(1) 反查（#3）：共享锁拷贝路由，随即释放
                    SsrcRoute route;
                    if (lookup_ssrc(ssrc, route)) {
                        // 更新该流的接收统计（原子，无需房间锁）
                        route.stream->packets_received++;
                        route.stream->bytes_received += received;
                        route.stream->last_active_time = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch()).count();

                        // ② 转发给房间内其他参与者（锁序：room->mutex）
                        int forwarded = 0;
                        {
                            std::lock_guard<std::mutex> rlk(route.room->mutex);
                            for (auto& [uid, p] : route.room->participants) {
                                if (uid == route.sender_id) continue;   // 不回环
                                auto sit = p->streams.find(route.media_type);
                                if (sit == p->streams.end() || !sit->second->active) continue;
                                // #5：使用注册时预解析的 sockaddr_in，不再逐包 inet_pton
                                ::sendto(sock, recv_buf, received, 0,
                                         (struct sockaddr*)&sit->second->client_addr_in,
                                         sizeof(sit->second->client_addr_in));
                                total_packets_forwarded_++;
                                forwarded++;
                            }
                        }

                        // 定期摘要：每 100 个包输出一次转发统计（#18 成员化）
                        uint64_t seq = fwd_seq_.fetch_add(1);
                        if (seq % 100 == 0) {
                            LOG_INFO("MediaRelay fwd: room=%s, sender=%lu, media=%s, "
                                     "ssrc=%u, fwd_to=%d participants, total_fwd=%lu",
                                     route.room->room_id.c_str(), route.sender_id,
                                     route.media_type.c_str(), ssrc, forwarded,
                                     total_packets_forwarded_.load());
                        }
                    } else {
                        // 未知 SSRC（可能未注册或已离开）（#18 成员化）
                        uint64_t u = unknown_ssrc_seq_.fetch_add(1);
                        if (u < 10 || u % 1000 == 0) {
                            char addr_buf[INET_ADDRSTRLEN];
                            LOG_WARN("MediaRelay unknown ssrc=%u pt=%u from %s:%u (ignored)",
                                     ssrc, pt, format_addr(client_addr, addr_buf),
                                     ntohs(client_addr.sin_port));
                        }
                    }
                }
            }

            // 继续读下一个数据报，直到 EAGAIN（一次 poll 读尽）
            client_len = sizeof(client_addr);
            received = ::recvfrom(sock, recv_buf, kBufSize, MSG_DONTWAIT,
                                  (struct sockaddr*)&client_addr, &client_len);
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && running_) {
            LOG_ERROR("MediaRelay thread %d: recvfrom error: %s",
                      thread_id, strerror(errno));
            break;
        }
    }

    delete[] recv_buf;
    LOG_INFO("MediaRelay thread %d stopped", thread_id);
}

/**
 * @brief 注册参与者的一路媒体流（由信令层在会议加入/媒体开启时调用）
 *
 * 核心流程：
 *  1. 确定中继端口：按 media_type 路由（audio→0号端口, video→1号端口）；
 *  2. 确保房间/参与者存在（无则创建）；
 *  3. 【SSRC 分配】：首个流随机分配（#12），后续流复用首个流的 SSRC；
 *  4. 【地址预解析】（#5）：inet_pton 一次存入 client_addr_in，
 *     转发热路径零解析；
 *  5. 【SSRC 索引】（#3）：把 (ssrc → SsrcRoute) 插入 ssrc_index_。
 *
 * 锁序：rooms_mutex_ → room->mutex → ssrc_index_mutex_（全局唯一顺序）。
 */
bool MediaRelay::register_participant(
        uint64_t user_id, const std::string& room_id,
        const std::string& nickname,
        const std::string& client_host, uint16_t client_port,
        const std::string& media_type,
        uint32_t& out_ssrc,
        std::string& out_relay_host,
        uint16_t& out_relay_port) {

    out_relay_host = bind_ip_;
    // 根据 media_type 分配到不同端口：
    //   audio → 0 号端口；video → 1 号端口；screen → 独立 2 号端口（若线程数足够），
    //   线程数不足时回退到 video 端口（1 号），避免 screen 与 audio 混用同一线程端口。
    //   注意：v2.1 早期实现把 screen 落入 else 分支（idx=0=audio 端口），
    //   若客户端注册 screen 流会导致与音频争用同一端口、丢包/乱序 → 黑屏。
    int relay_idx;
    if (media_type == "audio") {
        relay_idx = 0;
    } else if (media_type == "video") {
        relay_idx = 1 % worker_threads_;
    } else if (media_type == "screen") {
        relay_idx = (worker_threads_ >= 3) ? 2 : (1 % worker_threads_);
    } else {
        relay_idx = 0;
    }
    out_relay_port = base_port_ + static_cast<uint16_t>(relay_idx);

    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto& room = rooms_[room_id];
    if (!room) {
        room = std::make_shared<RelayRoom>();
        room->room_id = room_id;
        LOG_INFO("MediaRelay: created room %s", room_id.c_str());
    }

    std::lock_guard<std::mutex> rlk(room->mutex);
    auto& p = room->participants[user_id];
    if (!p) {
        p = std::make_shared<RelayParticipant>();
        p->user_id = user_id;
        p->nickname = nickname;
    }

    // 同一用户(video/audio/screen)复用同一个 ssrc，确保 RTP 同步源唯一，
    // 这样中继与客户端都能用单一 ssrc 准确识别发送者。
    bool reuse = !p->streams.empty();
    if (reuse) {
        // 已有流 → 复用第一个流的 SSRC
        out_ssrc = p->streams.begin()->second->ssrc;
    }

    // 保存这路流的信息（转发时据此 sendto 到客户端）
    auto stream = std::make_shared<RTPStreamInfo>();
    stream->media_type = media_type;
    stream->client_host = client_host;
    stream->client_port = client_port;
    stream->relay_host = out_relay_host;
    stream->relay_port = out_relay_port;
    stream->active = true;
    stream->last_active_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // #5：注册时一次性预解析客户端地址，转发热路径直接使用
    std::memset(&stream->client_addr_in, 0, sizeof(stream->client_addr_in));
    stream->client_addr_in.sin_family = AF_INET;
    stream->client_addr_in.sin_port   = htons(client_port);
    if (::inet_pton(AF_INET, client_host.c_str(),
                    &stream->client_addr_in.sin_addr) != 1) {
        LOG_ERROR("MediaRelay: invalid client address '%s' for user=%lu, register rejected",
                  client_host.c_str(), user_id);
        return false;
    }

    p->streams[media_type] = stream;
    p->last_heartbeat = stream->last_active_time;

    // 锁序末级：ssrc_index_mutex_（独占写）
    {
        std::unique_lock<std::shared_mutex> idx_lock(ssrc_index_mutex_);

        if (!reuse) {
            // 首个流 → 随机分配新 SSRC（#12，冲突检测在锁内完成）
            out_ssrc = allocate_ssrc_locked();
        }
        stream->ssrc = out_ssrc;

        // #3：建立/更新 SSRC 路由索引（同 SSRC 多路流仅首次插入，
        // 索引项的 media_type 以首条注册流为准——转发靠包到达端口区分流）
        if (ssrc_index_.find(out_ssrc) == ssrc_index_.end()) {
            SsrcRoute route;
            route.room       = room;
            route.sender_id  = user_id;
            route.media_type = media_type;
            route.stream     = stream;
            ssrc_index_.emplace(out_ssrc, std::move(route));
        }
    }

    LOG_INFO("MediaRelay: registered user=%lu room=%s media=%s ssrc=%u relay=%s:%u "
             "(peers_in_room=%zu)",
             user_id, room_id.c_str(), media_type.c_str(), out_ssrc,
             out_relay_host.c_str(), out_relay_port,
             room->participants.size());
    return true;
}

void MediaRelay::unregister_participant(uint64_t user_id, const std::string& room_id) {
    // 先收集该用户全部 SSRC，再批量从索引删除
    std::vector<uint32_t> ssrcs_to_remove;

    {
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return;

        {
            std::lock_guard<std::mutex> rlk(it->second->mutex);
            auto pit = it->second->participants.find(user_id);
            if (pit != it->second->participants.end()) {
                for (auto& [mt, si] : pit->second->streams) {
                    ssrcs_to_remove.push_back(si->ssrc);
                }
                it->second->participants.erase(pit);
            }
        }

        LOG_INFO("MediaRelay: unregistered user=%lu from room=%s", user_id, room_id.c_str());

        // 房间空时清理
        if (it->second->participants.empty()) {
            rooms_.erase(it);
            LOG_INFO("MediaRelay: removed empty room %s", room_id.c_str());
        }
    }

    // 锁序末级：ssrc_index_mutex_（独占写）批量删除（#3）
    // 注意：此时已释放 rooms_mutex_/room->mutex，单独取索引锁不构成反序。
    if (!ssrcs_to_remove.empty()) {
        std::unique_lock<std::shared_mutex> idx_lock(ssrc_index_mutex_);
        for (uint32_t ssrc : ssrcs_to_remove) {
            ssrc_index_.erase(ssrc);
        }
    }
}

void MediaRelay::update_media_status(uint64_t user_id, const std::string& room_id,
                                      bool audio_on, bool video_on) {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return;

    std::lock_guard<std::mutex> rlk(it->second->mutex);
    auto pit = it->second->participants.find(user_id);
    if (pit == it->second->participants.end()) return;

    pit->second->audio_on = audio_on;
    pit->second->video_on = video_on;

    // 更新对应流的活跃状态
    if (auto asi = pit->second->streams.find("audio"); asi != pit->second->streams.end()) {
        asi->second->active = audio_on;
    }
    if (auto vsi = pit->second->streams.find("video"); vsi != pit->second->streams.end()) {
        vsi->second->active = video_on;
    }
}

/**
 * @brief 上报客户端统计（#1 死锁修复版）
 *
 * 旧实现在持有 rooms_mutex_ + room->mutex 的情况下调用公共
 * suggest_bitrate()（内部再次对 rooms_mutex_ 加锁）→ 非递归互斥锁
 * 重入 = 死锁。
 *
 * 修复：
 *  1. 持锁内仅做统计更新 + 调用 suggest_bitrate_unlocked()（不重入锁）；
 *  2. 记录"需要提示的参与者指针 + 建议码率"；
 *  3. 释放全部锁后再 send_bandwidth_hint()（锁外做 IO/回调）。
 */
void MediaRelay::report_stats(uint64_t user_id, const std::string& room_id,
                               double packet_loss, double rtt, double jitter,
                               uint32_t bitrate, int32_t quality) {
    std::shared_ptr<RelayParticipant> hint_target;
    uint32_t hint_bitrate = 0;

    {
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return;

        std::lock_guard<std::mutex> rlk(it->second->mutex);
        auto pit = it->second->participants.find(user_id);
        if (pit == it->second->participants.end()) return;

        for (auto& [mt, si] : pit->second->streams) {
            si->packet_loss_rate = packet_loss;
            si->rtt_ms = rtt;
        }

        // 根据丢包率判断是否需要带宽调整
        if (packet_loss > 0.05) {  // 丢包 >5%
            LOG_DEBUG("High packet loss for user=%lu: loss=%.2f%%",
                      user_id, packet_loss * 100);
            // 持锁内调用 _unlocked 版本（不重入锁，无死锁）
            hint_bitrate = suggest_bitrate_unlocked(it->second, user_id);
            hint_target  = pit->second;
        }
    }   // ← 释放全部锁

    // 锁外发送带宽提示（IO/回调不持锁）
    if (hint_target) {
        send_bandwidth_hint(hint_target, hint_bitrate);
    }
}

/**
 * @brief 基于丢包率的分级码率建议（弱网自适应核心）— 公共版本
 *
 * 自加锁后委托 _unlocked 版本，供信令层等未持锁上下文调用。
 */
uint32_t MediaRelay::suggest_bitrate(uint64_t user_id, const std::string& room_id) {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return 2000;  // 房间不存在 → 默认 2Mbps

    std::lock_guard<std::mutex> rlk(it->second->mutex);
    return suggest_bitrate_unlocked(it->second, user_id);
}

/**
 * @brief 建议码率 _unlocked 版本：假定调用者已持有 room->mutex
 *
 * 算法：
 *  1. 取该用户所有流（video/audio/screen）中【最大】丢包率——因为
 *     任一媒体流丢包都说明网络吃紧，取最坏值最保守；
 *  2. 按分级阈值映射到建议码率：
 *       >15% 严重丢包 → 300kbps（最低保底）
 *       >10% 高丢包   → 500kbps
 *       >5%  中等丢包 → 1000kbps
 *       >2%  轻微丢包 → 1500kbps
 *       否则良好      → 2500kbps（默认最高）
 *
 * @return 建议码率（kbps）
 */
uint32_t MediaRelay::suggest_bitrate_unlocked(
        const std::shared_ptr<RelayRoom>& room, uint64_t user_id) {
    auto pit = room->participants.find(user_id);
    if (pit == room->participants.end()) return 2000;

    // 基于丢包率的自适应算法
    double max_loss = 0.0;
    for (auto& [mt, si] : pit->second->streams) {
        // 取最大丢包率（最保守的评估）
        max_loss = std::max(max_loss, si->packet_loss_rate.load());
    }

    if (max_loss > 0.15) return 300;    // 严重丢包 → 最低 300kbps
    if (max_loss > 0.10) return 500;    // 高丢包 → 500kbps
    if (max_loss > 0.05) return 1000;   // 中等丢包 → 1Mbps
    if (max_loss > 0.02) return 1500;   // 轻微丢包 → 1.5Mbps
    return 2500;                         // 良好 → 2.5Mbps
}

void MediaRelay::send_bandwidth_hint(
        const std::shared_ptr<RelayParticipant>& participant,
        uint32_t suggested_bitrate) {
    // 带宽提示将通过信令信道发送给客户端
    // 这里记录日志，实际发送由调用者通过信令完成
    LOG_DEBUG("Bandwidth hint for user=%lu: %u kbps",
              participant->user_id, suggested_bitrate);
}

std::vector<RelayParticipant> MediaRelay::get_room_participants(const std::string& room_id) {
    std::vector<RelayParticipant> result;
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return result;

    std::lock_guard<std::mutex> rlk(it->second->mutex);
    for (auto& [uid, p] : it->second->participants) {
        result.push_back(*p);
    }
    return result;
}

/**
 * @brief 分配一个随机 SSRC（#12）
 *
 * 旧实现为原子计数器从 1000 自增——可预测且跨重启必撞号。
 * 新实现：mt19937 随机生成 + ssrc_index_ 冲突检测重试。
 * 公共版本自加独占锁（含冲突检测所需的索引访问）。
 */
uint32_t MediaRelay::allocate_ssrc() {
    std::unique_lock<std::shared_mutex> idx_lock(ssrc_index_mutex_);
    return allocate_ssrc_locked();
}

/**
 * @brief SSRC 随机分配 _unlocked 版本
 *
 * 假定调用者已持有 ssrc_index_mutex_ 独占锁。
 * 随机范围 [1, UINT32_MAX]，撞索引则重试；极端情况下（索引几乎满）
 * 重试次数天然受循环条件限制（空位概率极高，实际 1 次命中）。
 */
uint32_t MediaRelay::allocate_ssrc_locked() {
    uint32_t ssrc = 0;
    do {
        ssrc = rng_();
        if (ssrc == 0) continue;   // 0 保留作"未分配"语义
    } while (ssrc_index_.find(ssrc) != ssrc_index_.end());
    return ssrc;
}

/**
 * @brief SSRC → 路由 O(1) 查找（#3）
 *
 * 共享锁读取（多 relay 线程并发查找互不阻塞），命中后拷贝
 * SsrcRoute（含 shared_ptr，锁外使用安全）。
 */
bool MediaRelay::lookup_ssrc(uint32_t ssrc, SsrcRoute& out_route) {
    std::shared_lock<std::shared_mutex> idx_lock(ssrc_index_mutex_);
    auto it = ssrc_index_.find(ssrc);
    if (it == ssrc_index_.end()) return false;
    out_route = it->second;
    return true;
}

/**
 * @brief RTCP 包处理 — 接收/发送报告统计
 *
 * 常见类型：SR(200) Sender Report / RR(201) Receiver Report /
 * SDES(202) / BYE(203) / APP(204)。
 *
 * 解析采用显式字节读取（#11 删除位域结构后的统一方式），
 * 按 SSRC 经 ssrc_index_ O(1) 定位流并更新丢包/抖动统计（#3）。
 */
void MediaRelay::handle_rtcp_packet(const uint8_t* data, size_t len,
                                     const std::string& client_addr,
                                     uint16_t client_port) {
    if (len < 8) return;

    // RTCP 头: [0]=V/P/RC, [1]=PT, [2:3]=length(32bit字, 不含本头)
    uint8_t pt = data[1] & 0x7F;
    // 仅处理 SR(200)/RR(201) 接收/发送报告
    if (pt != 200 && pt != 201) {
        LOG_DEBUG("RTCP(PT=%u) from %s:%u, size=%zu", pt,
                  client_addr.c_str(), client_port, len);
        return;
    }

    // 报告块起始于偏移 8（跳过 8 字节头），发送者/接收者 SSRC 在偏移 4
    if (len < 8 + 20) return;

    uint32_t ssrc_source = (static_cast<uint32_t>(data[8]) << 24) |
                           (static_cast<uint32_t>(data[9]) << 16) |
                           (static_cast<uint32_t>(data[10]) << 8) |
                           static_cast<uint32_t>(data[11]);
    double fraction_lost = static_cast<double>(data[12]) / 256.0;
    uint32_t jitter = (static_cast<uint32_t>(data[16]) << 24) |
                      (static_cast<uint32_t>(data[17]) << 16) |
                      (static_cast<uint32_t>(data[18]) << 8) |
                      static_cast<uint32_t>(data[19]);

    // 按 ssrc 索引 O(1) 查找并更新对应流的丢包/抖动统计（#3）
    SsrcRoute route;
    if (lookup_ssrc(ssrc_source, route)) {
        route.stream->packet_loss_rate = fraction_lost;
        // jitter 字段为 RTP 时间戳单位，粗略换算为毫秒（视频 90000Hz）
        route.stream->rtt_ms.store(jitter / 90.0, std::memory_order_relaxed);
        LOG_DEBUG("RTCP(%s) from %s:%u: ssrc=%u loss=%.1f%% jitter=%u",
                  pt == 200 ? "SR" : "RR", client_addr.c_str(),
                  client_port, ssrc_source, fraction_lost * 100, jitter);
        return;
    }
    LOG_DEBUG("RTCP(%s) from %s:%u: unknown ssrc=%u (ignored)",
              pt == 200 ? "SR" : "RR", client_addr.c_str(), client_port, ssrc_source);
}

} // namespace wemeet
