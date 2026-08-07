#pragma once
/**
 * @file rtp_header.h
 * @brief RTP 头部唯一定义 — 服务端（media_relay）与客户端（rtp_session）共享
 *
 * 设计约束：
 *   - 纯 C++17、无 Qt 依赖，server/client 均可包含；
 *   - 保持 12 字节紧凑布局（#pragma pack(1)），与线上协议完全一致；
 *   - static_assert(sizeof == 12) 编译期防回归；
 *   - validate_rtp_packet() 提供收包路径的最小合法性校验（#17）。
 */

#include <cstdint>
#include <cstddef>

#if defined(_WIN32)
#  include <winsock2.h>   // htons/htonl/ntohs/ntohl
#else
#  include <arpa/inet.h>
#endif

namespace wemeet {

/**
 * @brief RTP 媒体包头部（标准 RTPv2 格式 12 字节）
 *
 * ─────────────────────────────────────────────────────────────
 *  SSRC 深度解释（中继转发的核心标识）
 * ─────────────────────────────────────────────────────────────
 *  SSRC = Synchronization Source（同步源标识），是一个 32 位随机数，
 *  用于唯一标识【一个媒体流】（如某人的视频、某人的音频）。
 *
 *  它在本项目 SFU 转发里扮演"身份 + 路由"的双重角色：
 *   - 身份：同一发送者产生的所有 RTP 包携带同一个 SSRC，接收端靠它
 *           把不同用户的流区分开；
 *   - 路由：中继收包后，用"包的 SSRC"经 ssrc_index_ 哈希表 O(1) 反查
 *           "它属于哪个用户 → 哪个房间 → 要转发给哪些人"。
 *
 *  与 TCP 的对比（帮助理解）：
 *   - TCP 用"四元组(源IP:端口, 目的IP:端口)"区分连接；
 *   - UDP 无连接，RTP 用 SSRC 做应用层标识，即使同 IP:端口
 *     发出多个流，也能靠 SSRC 区分。
 */
#pragma pack(push, 1)
struct RTPHeader {
    uint8_t  cc_version;       // [0:3] CSRC count, [4:5] version=2, [6] padding, [7] extension
    uint8_t  pt_marker;        // [0:6] payload type, [7] marker
    uint16_t sequence_number;  // 序列号（网络字节序）：检测丢包/乱序
    uint32_t timestamp;        // 时间戳（网络字节序）：播放时序/抖动
    uint32_t ssrc;             // 同步源标识（网络字节序）：媒体流唯一ID

    // ── 编码辅助（主机序 → 网络序）────────────────────
    void set_version() { cc_version = (cc_version & 0x0F) | (2 << 6); }
    void set_marker(bool m) { pt_marker = (pt_marker & 0x7F) | (m ? 0x80 : 0); }
    void set_payload_type(uint8_t pt) { pt_marker = (pt_marker & 0x80) | (pt & 0x7F); }
    void set_sequence(uint16_t seq) { sequence_number = htons(seq); }
    void set_timestamp(uint32_t ts) { timestamp = htonl(ts); }
    void set_ssrc(uint32_t id) { ssrc = htonl(id); }

    // ── 解码辅助（网络序 → 主机序）─────────────────────
    uint8_t  version() const { return (cc_version >> 6) & 0x03; }
    uint8_t  payload_type() const { return pt_marker & 0x7F; }
    bool     marker() const { return (pt_marker & 0x80) != 0; }
    uint16_t sequence() const { return ntohs(sequence_number); }
    uint32_t timestamp_val() const { return ntohl(timestamp); }
    uint32_t ssrc_val() const { return ntohl(ssrc); }

    static constexpr size_t kHeaderSize = 12;   // RTPv2 固定头 12 字节
};
#pragma pack(pop)

// 编译期保证：头部布局必须严格等于 12 字节（协议兼容性硬约束）
static_assert(sizeof(RTPHeader) == RTPHeader::kHeaderSize,
              "RTPHeader must be exactly 12 bytes (RTPv2 fixed header)");

/**
 * @brief 收包路径最小合法性校验（#17）
 *
 * 校验项：
 *   1. 包长 >= RTP 固定头（12 字节），否则连头部都读不全；
 *   2. version 字段必须等于 2（RTPv2），防止把乱码/其他协议当 RTP 处理。
 *
 * 说明：CSRC 扩展头（cc > 0）场景下真实头更长，本项目不产生/不接收
 * 带 CSRC 的流，故仅做固定头校验。
 *
 * @param data 原始包数据
 * @param len  包长度
 * @return true = 合法 RTPv2 包
 */
inline bool validate_rtp_packet(const uint8_t* data, size_t len) {
    if (!data || len < RTPHeader::kHeaderSize) return false;
    const auto* hdr = reinterpret_cast<const RTPHeader*>(data);
    return hdr->version() == 2;
}

} // namespace wemeet
