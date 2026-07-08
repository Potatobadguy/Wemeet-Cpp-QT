/**
 * @brief 单元测试 — Buffer 移动语义
 */

#include "buffer.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace wemeet;

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ── 测试1：基础读写 ─────────────────────────────────────
static void test_basic_rw() {
    TEST("basic read/write");

    Buffer buf;
    ASSERT(buf.readable_size() == 0, "should be empty initially");

    std::string data = "Hello, WeMeet!";
    buf.append(data.data(), data.size());
    ASSERT(buf.readable_size() == data.size(), "size mismatch after append");

    std::string retrieved = buf.retrieve_as_string(data.size());
    ASSERT(retrieved == data, "data mismatch");
    ASSERT(buf.readable_size() == 0, "should be empty after retrieve");

    PASS();
}

// ── 测试2：移动构造（右值引用）──────────────────────────
static void test_move_constructor() {
    TEST("move constructor (rvalue reference)");

    Buffer buf1;
    std::string test_data = "move semantics test data";
    buf1.append(test_data.data(), test_data.size());

    size_t orig_size = buf1.readable_size();
    const uint8_t* orig_data = buf1.data();

    // 移动构造 — 数据指针应保持不变
    Buffer buf2(std::move(buf1));

    ASSERT(buf1.readable_size() == 0, "moved-from buffer should be empty");
    ASSERT(buf2.readable_size() == orig_size, "moved-to buffer should have data");
    ASSERT(buf2.data() == orig_data, "data pointer should survive move");

    PASS();
}

// ── 测试3：移动赋值 ─────────────────────────────────────
static void test_move_assignment() {
    TEST("move assignment");

    Buffer buf1;
    buf1.append("AAA", 3);

    Buffer buf2;
    buf2.append("BBBBBB", 6);

    // 移动赋值
    buf2 = std::move(buf1);

    ASSERT(buf1.readable_size() == 0, "source should be empty");
    ASSERT(buf2.readable_size() == 3, "target should have source data");

    PASS();
}

// ── 测试4：Buffer::from_vector（移动语义构造）───────────
static void test_from_vector() {
    TEST("Buffer::from_vector (move-from vector)");

    std::vector<uint8_t> vec = {0xDE, 0xAD, 0xBE, 0xEF};
    Buffer buf = Buffer::from_vector(std::move(vec));

    ASSERT(buf.readable_size() == 4, "size mismatch");
    ASSERT(buf.data()[0] == 0xDE, "data mismatch");
    ASSERT(buf.data()[3] == 0xEF, "data mismatch");

    PASS();
}

// ── 测试5：prepend（协议头）─────────────────────────────
static void test_prepend() {
    TEST("prepend for protocol header");

    Buffer buf;
    buf.append("payload_data", 12);

    // 预留空间应该足够
    uint32_t header = 0x12345678;
    buf.prepend(&header, sizeof(header));

    ASSERT(buf.readable_size() == 16, "size should be payload + header");

    uint32_t read_header;
    std::memcpy(&read_header, buf.data(), 4);
    ASSERT(read_header == header, "header mismatch");

    PASS();
}

// ── main ─────────────────────────────────────────────────
namespace wemeet_test {

int test_buffer() {
    printf("\n  === Buffer (Move Semantics) Unit Tests ===\n\n");

    tests_passed = 0;
    tests_failed = 0;

    test_basic_rw();
    test_move_constructor();
    test_move_assignment();
    test_from_vector();
    test_prepend();

    printf("\n  Buffer: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
