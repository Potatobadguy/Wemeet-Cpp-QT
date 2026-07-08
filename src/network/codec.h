#pragma once
#include "buffer.h"
#include "common.pb.h"
#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

namespace wemeet {

/**
 * @brief Protobuf 编解码器 — 4字节大端长度头 + Protobuf body
 *
 * 协议格式:
 *   ┌─────────────┬──────────────────────────────┐
 *   │ 4 bytes     │ body_len bytes               │
 *   │ big-endian  │ protobuf serialized data     │
 *   │ body length │                              │
 *   └─────────────┴──────────────────────────────┘
 */
class Codec {
public:
    static constexpr size_t kHeaderLen = 4;
    static constexpr uint32_t kMaxBodyLen = 64 * 1024 * 1024;  // 64MB

    // ── 从 Buffer 中 peek 消息体长度 ─────────────────────
    static uint32_t peek_length(const Buffer& buf) {
        if (buf.readable_size() < kHeaderLen) return 0;
        uint32_t len;
        std::memcpy(&len, buf.peek(), kHeaderLen);
        return ntohl(len);   // 网络字节序 → 主机字节序
    }

    // ── 编码：消息 → Buffer（含长度头）──────────────────
    template <typename MsgType>
    static Buffer encode(const MsgType& msg) {
        std::string body = msg.SerializeAsString();

        Buffer buf;
        uint32_t net_len = htonl(static_cast<uint32_t>(body.size()));
        buf.append(&net_len, kHeaderLen);           // 长度头
        buf.append(body.data(), body.size());       // body

        return buf;
    }

    // ── 编码为 BaseMessage 包装 ──────────────────────────
    template <typename InnerMsg>
    static Buffer encode_wrapped(int msg_type, uint64_t seq_id, const InnerMsg& inner) {
        BaseMessage base;
        base.set_type(static_cast<MsgType>(msg_type));
        base.set_sequence_id(seq_id);
        base.set_timestamp_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        base.set_payload(inner.SerializeAsString());

        return encode(base);
    }

    // ── 解码：Buffer → BaseMessage ───────────────────────
    static bool decode_base_message(const Buffer& buf, BaseMessage& msg) {
        return msg.ParseFromArray(buf.peek(), buf.readable_size());
    }

    // ── 从 BaseMessage 中提取子消息 ──────────────────────
    template <typename InnerMsg>
    static bool decode_payload(const BaseMessage& base, InnerMsg& inner) {
        return inner.ParseFromString(base.payload());
    }
};

} // namespace wemeet
