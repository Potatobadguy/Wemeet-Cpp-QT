/**
 * @brief 单元测试 — Protobuf 编解码
 */
#include "codec.h"
#include "common.pb.h"
#include "auth.pb.h"
#include <cstdio>

using namespace wemeet;

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ── 测试1：长度头编解码 ─────────────────────────────────
static void test_length_header() {
    TEST("length header encode (4-byte big-endian)");

    BaseMessage msg;
    msg.set_type(MsgType::MSG_HEARTBEAT_REQ);
    msg.set_sequence_id(1);
    msg.set_timestamp_ms(12345);

    Buffer buf = Codec::encode(msg);

    // 验证长度头
    ASSERT(buf.readable_size() >= 4, "should have header");
    uint32_t peeked = Codec::peek_length(buf);
    ASSERT(peeked == buf.readable_size() - 4, "length header should match body size");

    PASS();
}

// ── 测试2：BaseMessage 编解码 ───────────────────────────
static void test_base_message() {
    TEST("BaseMessage encode + decode");

    BaseMessage msg;
    msg.set_type(MsgType::MSG_LOGIN_REQ);
    msg.set_sequence_id(42);
    msg.set_timestamp_ms(99999);
    msg.set_payload("test_payload_data");

    Buffer buf = Codec::encode(msg);
    buf.retrieve(4);  // 跳过长度头

    BaseMessage decoded;
    ASSERT(Codec::decode_base_message(buf, decoded), "decode should succeed");
    ASSERT(decoded.type() == MsgType::MSG_LOGIN_REQ, "type mismatch");
    ASSERT(decoded.sequence_id() == 42, "sequence_id mismatch");
    ASSERT(decoded.payload() == "test_payload_data", "payload mismatch");

    PASS();
}

// ── 测试3：包装编解码 ───────────────────────────────────
static void test_wrapped_message() {
    TEST("encode_wrapped (BaseMessage + inner message)");

    LoginReq login;
    login.set_email("test@wemeet.com");
    login.set_password("secret123");

    Buffer buf = Codec::encode_wrapped(
        static_cast<int>(MsgType::MSG_LOGIN_REQ), 100, login);

    ASSERT(buf.readable_size() > 4, "should contain header + body");

    // 解包
    buf.retrieve(4);
    BaseMessage base;
    ASSERT(Codec::decode_base_message(buf, base), "decode base failed");
    ASSERT(base.type() == MsgType::MSG_LOGIN_REQ, "type mismatch");

    LoginReq decoded_login;
    ASSERT(Codec::decode_payload(base, decoded_login), "decode inner failed");
    ASSERT(decoded_login.email() == "test@wemeet.com", "email mismatch");
    ASSERT(decoded_login.password() == "secret123", "password mismatch");

    PASS();
}

// ── 测试4：大消息 ────────────────────────────────────────
static void test_large_message() {
    TEST("large message encode/decode (10KB payload)");

    std::string large_payload(10240, 'X');

    BaseMessage msg;
    msg.set_type(MsgType::MSG_CHAT_SEND);
    msg.set_sequence_id(1);
    msg.set_payload(large_payload);

    Buffer buf = Codec::encode(msg);
    ASSERT(Codec::peek_length(buf) == buf.readable_size() - 4, "length mismatch");

    buf.retrieve(4);
    BaseMessage decoded;
    ASSERT(Codec::decode_base_message(buf, decoded), "decode failed");
    ASSERT(decoded.payload() == large_payload, "payload mismatch");

    PASS();
}

// ── 模块入口 ─────────────────────────────────────────────
namespace wemeet_test {

int test_codec() {
    printf("\n  === Protobuf Codec Unit Tests ===\n\n");

    tests_passed = tests_failed = 0;

    test_length_header();
    test_base_message();
    test_wrapped_message();
    test_large_message();

    printf("\n  Codec: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
