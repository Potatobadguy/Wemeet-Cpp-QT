#include "media_relay.h"
#include "logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>

namespace wemeet {

// ── RelayRoom 广播方法 ──────────────────────────────────────
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

    // 关闭所有 socket 以唤醒线程
    for (int fd : relay_sockets_) {
        if (fd >= 0) ::close(fd);
    }
    relay_sockets_.clear();

    for (auto& t : relay_threads_) {
        if (t.joinable()) t.join();
    }
    relay_threads_.clear();

    LOG_INFO("MediaRelay stopped, total packets forwarded: %lu",
             total_packets_forwarded_.load());
}

void MediaRelay::relay_thread_func(int thread_id, uint16_t port) {
    // 创建 UDP socket
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
        std::lock_guard<std::mutex> lock(rooms_mutex_);
        relay_sockets_.push_back(sock);
    }

    // 接收缓冲区（最大支持 64KB 媒体包）
    constexpr size_t kBufSize = 65536;
    auto* recv_buf = new uint8_t[kBufSize];

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    LOG_INFO("MediaRelay thread %d listening on UDP port %u", thread_id, port);

    while (running_) {
        // 非阻塞接收 + 忙等（简化实现，生产环境应使用 epoll/select）
        client_len = sizeof(client_addr);
        ssize_t received = ::recvfrom(sock, recv_buf, kBufSize, MSG_DONTWAIT,
                                      (struct sockaddr*)&client_addr, &client_len);

        if (received > 0) {
            // DEBUG: log first packet
            static std::atomic<int> debug_count{0};
            if (debug_count.fetch_add(1) < 20) {
                LOG_INFO("MediaRelay[%d] port=%u received %zd bytes from %s:%u",
                         thread_id, port, received,
                         inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            }
            // 检查是否为 RTCP (payload type >= 200)
            if (received >= static_cast<ssize_t>(RTPHeader::kHeaderSize)) {
                auto* hdr = reinterpret_cast<RTPHeader*>(recv_buf);
                uint8_t pt = hdr->payload_type();

                if (pt >= 200 && pt <= 204) {
                    // RTCP 包 → 处理统计
                    handle_rtcp_packet(recv_buf, received,
                                       inet_ntoa(client_addr.sin_addr),
                                       ntohs(client_addr.sin_port));
                } else if (pt < 200) {
                    // RTP 媒体包 → 根据 media_type 转发
                    uint32_t ssrc = hdr->ssrc_val();

                    // 查找发送者所属房间
                    std::string found_room;
                    uint64_t sender_id = 0;
                    std::string media_type;

                    {
                        std::lock_guard<std::mutex> lk(rooms_mutex_);
                        for (auto& [rid, room] : rooms_) {
                            std::lock_guard<std::mutex> rlk(room->mutex);
                            for (auto& [uid, p] : room->participants) {
                                for (auto& [mt, si] : p->streams) {
                                    if (si->ssrc == ssrc) {
                                        sender_id = uid;
                                        media_type = mt;
                                        found_room = rid;
                                        si->packets_received++;
                                        si->bytes_sent += received;
                                        si->last_active_time = std::chrono::duration_cast<
                                            std::chrono::milliseconds>(
                                                std::chrono::system_clock::now()
                                                    .time_since_epoch()).count();
                                        goto found;
                                    }
                                }
                            }
                        }
                    }
                    found:
                    if (!found_room.empty() && sender_id > 0) {
                        // 转发给房间内其他参与者
                        auto room = get_room_participants(found_room);
                        // 使用 UDP socket 转发
                        std::lock_guard<std::mutex> lk(rooms_mutex_);
                        auto rit = rooms_.find(found_room);
                        if (rit != rooms_.end()) {
                            rit->second->broadcast_data(
                                sender_id, media_type, recv_buf, received,
                                [this, sock](uint64_t uid, const uint8_t* data, size_t len) {
                                    // 在房间表中查找目标地址
                                    // (简化：发送者的信息在 participant 的 stream 中)
                                    total_packets_forwarded_++;
                                });

                            // 简化转发: 直接发送给每个参与者的注册地址
                            std::lock_guard<std::mutex> rlk(rit->second->mutex);
                            for (auto& [uid, p] : rit->second->participants) {
                                if (uid == sender_id) continue;
                            for (auto& [mt, si] : p->streams) {
                                if (mt == media_type) {
                                    struct sockaddr_in dest;
                                    std::memset(&dest, 0, sizeof(dest));
                                    dest.sin_family = AF_INET;
                                    inet_pton(AF_INET, si->client_host.c_str(), &dest.sin_addr);
                                    dest.sin_port = htons(si->client_port);
                                    ::sendto(sock, recv_buf, received, 0,
                                             (struct sockaddr*)&dest, sizeof(dest));
                                    total_packets_forwarded_++;
                                    break;
                                }
                            }
                            }
                        }
                    }
                }
            }

            // 调用回调（用于信令集成）
            if (packet_cb_ && received >= static_cast<ssize_t>(RTPHeader::kHeaderSize)) {
                auto* hdr = reinterpret_cast<RTPHeader*>(recv_buf);
                if (hdr->payload_type() < 200) {
                    std::string mt = (hdr->payload_type() < 100) ? "audio" : "video";
                    // packet_cb_ 回调...
                }
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (running_) {
                LOG_ERROR("MediaRelay thread %d: recvfrom error: %s",
                          thread_id, strerror(errno));
            }
            break;
        } else {
            // 无数据，睡眠减少 CPU 使用
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    delete[] recv_buf;
    LOG_INFO("MediaRelay thread %d stopped", thread_id);
}

bool MediaRelay::register_participant(
        uint64_t user_id, const std::string& room_id,
        const std::string& nickname,
        const std::string& client_host, uint16_t client_port,
        const std::string& media_type,
        uint32_t& out_ssrc,
        std::string& out_relay_host,
        uint16_t& out_relay_port) {

    out_relay_host = bind_ip_;
    // 根据 media_type 分配到不同端口
    int relay_idx = (media_type == "audio") ? 0 :
                    (media_type == "video") ? 1 % worker_threads_ : 0;
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
    if (!p->streams.empty()) {
        out_ssrc = p->streams.begin()->second->ssrc;
    } else {
        out_ssrc = allocate_ssrc();
    }

    auto stream = std::make_shared<RTPStreamInfo>();
    stream->ssrc = out_ssrc;
    stream->media_type = media_type;
    stream->client_host = client_host;
    stream->client_port = client_port;
    stream->relay_host = out_relay_host;
    stream->relay_port = out_relay_port;
    stream->active = true;
    stream->last_active_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    p->streams[media_type] = stream;
    p->last_heartbeat = stream->last_active_time;

    LOG_INFO("MediaRelay: registered user=%lu room=%s media=%s ssrc=%u relay=%s:%u",
             user_id, room_id.c_str(), media_type.c_str(), out_ssrc,
             out_relay_host.c_str(), out_relay_port);
    return true;
}

void MediaRelay::unregister_participant(uint64_t user_id, const std::string& room_id) {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return;

    std::lock_guard<std::mutex> rlk(it->second->mutex);
    it->second->participants.erase(user_id);

    LOG_INFO("MediaRelay: unregistered user=%lu from room=%s", user_id, room_id.c_str());

    // 房间空时清理
    if (it->second->participants.empty()) {
        rooms_.erase(it);
        LOG_INFO("MediaRelay: removed empty room %s", room_id.c_str());
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

void MediaRelay::report_stats(uint64_t user_id, const std::string& room_id,
                               double packet_loss, double rtt, double jitter,
                               uint32_t bitrate, int32_t quality) {
    // 更新参与者流统计信息
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
        LOG_DEBUG("High packet loss for user=%lu: loss=%.2f%%", user_id, packet_loss * 100);
        // 带宽调整提示
        send_bandwidth_hint(pit->second, suggest_bitrate(user_id, room_id));
    }
}

uint32_t MediaRelay::suggest_bitrate(uint64_t user_id, const std::string& room_id) {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) return 2000;  // 默认 2Mbps

    std::lock_guard<std::mutex> rlk(it->second->mutex);
    auto pit = it->second->participants.find(user_id);
    if (pit == it->second->participants.end()) return 2000;

    // 基于丢包率的自适应算法
    double max_loss = 0.0;
    for (auto& [mt, si] : pit->second->streams) {
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

uint32_t MediaRelay::allocate_ssrc() {
    return next_ssrc_++;
}

void MediaRelay::handle_rtcp_packet(const uint8_t* data, size_t len,
                                     const std::string& client_addr,
                                     uint16_t client_port) {
    // RTCP 包处理 — 解析接收报告更新统计
    if (len < 8) return;
    // RR: Receiver Report (payload type 201)
    // SR: Sender Report (payload type 200)
    // SDES: Source Description (payload type 202)
    // BYE: Goodbye (payload type 203)
    // APP: Application-defined (payload type 204)
    LOG_DEBUG("RTCP packet received from %s:%u, size=%zu", client_addr.c_str(), client_port, len);
}

} // namespace wemeet
