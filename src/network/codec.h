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
 * ─────────────────────────────────────────────────────────────
 *  为什么需要"分帧"（framing）？
 * ─────────────────────────────────────────────────────────────
 *  TCP 是【字节流】协议，没有消息边界。客户端连续发送两条消息
 *  A 和 B，接收方可能一次性收到 "AB"、也可能收到 "A 的一半+B"、
 *  还可能拆成任意字节数。因此应用层必须自己约定"一条消息到哪为止"，
 *  这个约定就叫分帧。
 *
 *  本协议采用最经典的方案：**长度前缀（Length-Prefix）**——
 *  每条消息 = 4 字节长度头 + 变长 body。
 *
 *  协议格式:
 *   ┌─────────────┬──────────────────────────────┐
 *   │ 4 bytes     │ body_len bytes               │
 *   │ big-endian  │ protobuf serialized data     │
 *   │ body length │                              │
 *   └─────────────┴──────────────────────────────┘
 *
 *  ─────────────────────────────────────────────────────────────
 *  字节序说明
 * ─────────────────────────────────────────────────────────────
 *   - 网络传输统一使用【大端字节序】（网络字节序，即 "ABC" 顺序）。
 *   - 编码侧：htons/htonl（主机序 → 网络序）
 *   - 解码侧：ntohs/ntohl（网络序 → 主机序）
 *   - 这样无论收发两端主机是 x86 小端还是 ARM 大端，
 *     长度头都能被正确解析（跨平台互操作）。
 *
 *  ─────────────────────────────────────────────────────────────
 *  与 Buffer / TcpConnection 的配合
 * ─────────────────────────────────────────────────────────────
 *  本类只负责"二进制 ↔ 消息对象"的转换，不关心网络收发：
 *   - 编码结果写入 Buffer（内存中连续的可读字节流）
 *   - TcpConnection::handle_read 收到字节后调用本类解码
 *   - 解码依赖 Buffer 中已有的字节数判断消息是否完整
 *   （详见 tcp_connection.cpp 的解码循环）
 */
class Codec {
public:
    // 长度头字节数：固定 4 字节大端无符号整数
    static constexpr size_t kHeaderLen = 4;
    // 单条消息 body 上限：64MB，防止恶意/异常客户端发送超长消息
    // 撑爆内存（tcp_connection.cpp 解码处也会用此常量做防御校验）
    static constexpr uint32_t kMaxBodyLen = 64 * 1024 * 1024;  // 64MB

    // ── 从 Buffer 中 peek 消息体长度 ─────────────────────
    /**
     * @brief 偷看（peek）长度头，得到消息体长度，不消费 Buffer 数据
     *
     * 用途：
     *  调用方（TcpConnection::handle_read）先用它判断"是否收到了一整条
     *  消息"：若 Buffer 中字节数 < 4 + body_len，说明数据不完整，
     *  继续等待更多字节；反之说明已有一条完整消息可解码。
     *
     * 实现细节：
     *  - 长度不足 4 字节时返回 0（由调用方视为"数据不完整"）。
     *  - 用 std::memcpy 读取而非指针强转：
     *      `*(const uint32_t*)buf.peek()` 需要 4 字节对齐，
     *      而 Buffer 内部指针不一定对齐，memcpy 可安全跨平台。
     *  - ntohl：网络字节序（大端）→ 主机字节序。
     *
     * @param buf 收到的字节缓冲（不修改其内部读写指针）
     * @return body 长度（网络序转主机序后），不足 4 字节时返回 0
     */
    static uint32_t peek_length(const Buffer& buf) {
        if (buf.readable_size() < kHeaderLen) return 0;
        uint32_t len;
        std::memcpy(&len, buf.peek(), kHeaderLen);
        return ntohl(len);   // 网络字节序 → 主机字节序
    }

    // ── 编码：消息 → Buffer（含长度头）──────────────────
    /**
     * @brief 将任意 Protobuf 消息序列化并加上长度头，打包成 Buffer
     *
     * 模板函数：MsgType 可以是任意 protobuf 生成的消息类型
     * （LoginRequest、BaseMessage 等），编译期按具体类型实例化。
     *
     * 流程：
     *  1. msg.SerializeAsString()：protobuf 序列化为二进制 string
     *  2. htonl(body.size())：长度转网络字节序（大端）
     *  3. 依次 append 长度头 + body，形成一条完整分帧消息
     *
     * @tparam MsgType protobuf 消息类型
     * @param msg 待发送的消息对象
     * @return 已含长度头的 Buffer（可直接交给 TcpConnection::send）
     */
    template <typename MsgType>
    static Buffer encode(const MsgType& msg) {
        // 序列化 protobuf body
        std::string body = msg.SerializeAsString();

        Buffer buf;
        // 长度头：主机序 → 网络序（大端）
        uint32_t net_len = htonl(static_cast<uint32_t>(body.size()));
        buf.append(&net_len, kHeaderLen);           // 长度头
        buf.append(body.data(), body.size());       // body

        return buf;
    }

    // ── 编码为 BaseMessage 包装 ──────────────────────────
    /**
     * @brief 业务消息编码：先包装进统一信封 BaseMessage，再整体编码
     *
     * 为什么需要 BaseMessage 信封？
     *  服务器要处理登录、建会、入会、心跳……几十种消息。若每种消息
     *  都直接裸发，接收方必须先解析才知道"这是哪种消息"，无法统一
     *  路由。信封模式把【路由信息】和【业务数据】分开：
     *
     *   BaseMessage（信封）
     *   ├─ type         : 消息类型（MsgType 枚举），用于分发路由
     *   ├─ sequence_id  : 序列号，用于请求/响应配对（seq 对齐）
     *   ├─ timestamp_ms : 发送时刻（毫秒），用于延迟统计/超时判断
     *   └─ payload      : 具体业务消息（inner 序列化后的字节）
     *
     * 发送端：inner（如 LoginRequest）塞进 payload → encode()
     * 接收端：decode_base_message 拆信封 → 按 type 分发 →
     *         decode_payload 还原具体业务消息
     *
     * @tparam InnerMsg 具体业务消息类型（LoginRequest 等）
     * @param msg_type 消息类型枚举值（转成 proto 的 MsgType）
     * @param seq_id   请求序列号（应答时回填，实现请求-响应配对）
     * @param inner    具体业务消息对象
     * @return 含长度头的 BaseMessage 编码 Buffer
     */
    template <typename InnerMsg>
    static Buffer encode_wrapped(int msg_type, uint64_t seq_id, const InnerMsg& inner) {
        // 组装信封：填路由信息 + 时间戳 + 业务负载
        BaseMessage base;
        base.set_type(static_cast<MsgType>(msg_type));
        base.set_sequence_id(seq_id);
        // 记录发送时刻（单调时钟？此处用系统时钟），
        // 供对端统计 RTT 或做超时判断
        base.set_timestamp_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // 业务消息序列化后装入 payload 字段
        base.set_payload(inner.SerializeAsString());

        // 信封本身也是一条普通消息 → 复用 encode() 加长度头
        return encode(base);
    }

    // ── 解码：Buffer → BaseMessage ───────────────────────
    /**
     * @brief 从 Buffer 中解析出 BaseMessage 信封
     *
     * 注意：本函数【不】检查/消费长度头——调用方（TcpConnection）
     * 已通过 peek_length 确认消息完整，并负责 retrieve 掉已消费部分。
     * 这里只把剩余可读字节直接喂给 protobuf 解析。
     *
     * @param buf 包含完整一条消息的缓冲（可读区 = 纯 body）
     * @param msg 输出参数，解析成功的 BaseMessage
     * @return 是否解析成功（字节非法/截断时返回 false）
     */
    static bool decode_base_message(const Buffer& buf, BaseMessage& msg) {
        return msg.ParseFromArray(buf.peek(), buf.readable_size());
    }

    // ── 从 BaseMessage 中提取子消息 ──────────────────────
    /**
     * @brief 从 BaseMessage 信封中还原具体业务消息
     *
     * 与 encode_wrapped 配对：发送时"inner → payload"，
     * 接收时"payload → inner"。配合 type 字段即可安全地
     * 把消息分发到对应业务处理函数。
     *
     * @tparam InnerMsg 具体业务消息类型
     * @param base 已解析的信封
     * @param inner 输出参数，还原后的业务消息
     * @return 是否解析成功
     */
    template <typename InnerMsg>
    static bool decode_payload(const BaseMessage& base, InnerMsg& inner) {
        return inner.ParseFromString(base.payload());
    }
};

} // namespace wemeet
