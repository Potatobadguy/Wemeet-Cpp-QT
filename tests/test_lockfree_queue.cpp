/**
 * @brief 单元测试 — 无锁 SPSC 队列
 *
 * 验证：
 *   1. 基础 push/pop
 *   2. 满队列行为
 *   3. 单生产者-单消费者并发
 *   4. std::atomic 内存序正确性
 */

#include "lockfree_queue.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
#include <vector>

using namespace wemeet;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ── 测试1：基础 push/pop ────────────────────────────────
static void test_basic() {
    TEST("basic push/pop");

    SPSCQueue<int> q(16);

    ASSERT(q.empty(), "should be empty initially");

    ASSERT(q.try_push(42), "push should succeed");
    ASSERT(!q.empty(), "should not be empty after push");

    int val = 0;
    ASSERT(q.try_pop(val), "pop should succeed");
    ASSERT(val == 42, "popped value mismatch");
    ASSERT(q.empty(), "should be empty after pop");

    PASS();
}

// ── 测试2：满队列 ───────────────────────────────────────
static void test_full_queue() {
    TEST("full queue behavior");

    SPSCQueue<int> q(4);   // capacity=4, 有效容量=3

    ASSERT(q.try_push(1), "push 1");
    ASSERT(q.try_push(2), "push 2");
    ASSERT(q.try_push(3), "push 3");
    ASSERT(!q.try_push(4), "push 4 should fail (queue full)");

    int val;
    ASSERT(q.try_pop(val) && val == 1, "pop 1");
    ASSERT(q.try_push(4), "push 4 after pop should succeed");

    PASS();
}

// ── 测试3：顺序正确性 ──────────────────────────────────
static void test_ordering() {
    TEST("FIFO ordering");

    SPSCQueue<int> q(1024);
    for (int i = 0; i < 1000; ++i) {
        q.try_push(i);
    }

    for (int i = 0; i < 1000; ++i) {
        int val;
        ASSERT(q.try_pop(val), "pop should succeed");
        ASSERT(val == i, "FIFO order violation");
    }

    ASSERT(q.empty(), "should be empty at end");
    PASS();
}

// ── 测试4：SPSC 并发 ────────────────────────────────────
static void test_spsc_concurrent() {
    TEST("SPSC concurrent (1 producer, 1 consumer)");

    SPSCQueue<uint64_t> q(65536);   // 大容量，减少阻塞
    constexpr uint64_t kItems = 1000000;

    std::atomic<bool>   producer_done{false};
    std::atomic<uint64_t> sum_produced{0};
    std::atomic<uint64_t> sum_consumed{0};

    // 生产者线程
    std::thread producer([&]() {
        for (uint64_t i = 0; i < kItems; ++i) {
            q.push(i);
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    // 消费者线程
    std::thread consumer([&]() {
        uint64_t received = 0;
        while (received < kItems) {
            uint64_t val;
            if (q.try_pop(val)) {
                sum_consumed.fetch_add(val, std::memory_order_relaxed);
                received++;
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT(sum_produced.load() == sum_consumed.load(),
           "sum mismatch — data corruption or missing items");

    printf("  [%lu items transferred correctly]\n", kItems);
    PASS();
}

// ── main ─────────────────────────────────────────────────
namespace wemeet_test {

int test_lockfree_queue() {
    printf("\n  === LockFree SPSC Queue Unit Tests ===\n\n");

    tests_passed = 0;
    tests_failed = 0;

    test_basic();
    test_full_queue();
    test_ordering();
    test_spsc_concurrent();

    printf("\n  LockFree Queue: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
