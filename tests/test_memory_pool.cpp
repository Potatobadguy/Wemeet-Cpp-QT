/**
 * @brief 单元测试 — 三级内存池
 *
 * 验证：
 *   1. 分配/释放基础功能
 *   2. 池命中（第二次分配从池中获取，不调 malloc）
 *   3. 多线程并发安全
 */

#include "memory_pool.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>

using namespace wemeet;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        printf("  TEST: %s ... ", name); \
    } while(0)

#define PASS() \
    do { \
        printf("PASSED\n"); \
        tests_passed++; \
    } while(0)

#define FAIL(msg) \
    do { \
        printf("FAILED: %s\n", msg); \
        tests_failed++; \
    } while(0)

#define ASSERT(cond, msg) \
    if (!(cond)) { FAIL(msg); return; }

// ── 测试1：基础分配释放 ─────────────────────────────────
static void test_basic_alloc() {
    TEST("basic allocate/deallocate");

    auto& pool = MemoryPool::instance();

    void* p = pool.allocate(MemoryPool::Tier::SMALL);
    ASSERT(p != nullptr, "allocation failed");

    // 写入测试数据
    uint8_t* data = static_cast<uint8_t*>(p);
    for (int i = 0; i < 1024; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    pool.deallocate(p, MemoryPool::Tier::SMALL);

    // 第二次分配（应该池命中）
    void* p2 = pool.allocate(MemoryPool::Tier::SMALL);
    ASSERT(p2 != nullptr, "second allocation failed");
    pool.deallocate(p2, MemoryPool::Tier::SMALL);

    PASS();
}

// ── 测试2：池命中验证 ───────────────────────────────────
static void test_pool_hit() {
    TEST("pool hit rate");

    auto& pool = MemoryPool::instance();
    auto  before = pool.stats();

    // 分配再释放 10 次，应该全部池命中
    constexpr int N = 10;
    void* ptrs[N];

    for (int i = 0; i < N; ++i) {
        ptrs[i] = pool.allocate(MemoryPool::Tier::MEDIUM);
    }
    for (int i = 0; i < N; ++i) {
        pool.deallocate(ptrs[i], MemoryPool::Tier::MEDIUM);
    }

    // 第二轮分配（应该全部池命中）
    void* ptrs2[N];
    for (int i = 0; i < N; ++i) {
        ptrs2[i] = pool.allocate(MemoryPool::Tier::MEDIUM);
    }
    for (int i = 0; i < N; ++i) {
        pool.deallocate(ptrs2[i], MemoryPool::Tier::MEDIUM);
    }

    auto after = pool.stats();
    uint64_t hits = after.pool_hits - before.pool_hits;

    // 第一次分配不计入命中（池为空），后续应该命中
    ASSERT(hits >= static_cast<uint64_t>(N - 1),
           "pool hit count too low (expected most allocs from pool)");

    printf("  [pool hits: %lu / %d allocs]\n", hits, N);
    PASS();
}

// ── 测试3：不同 Tier ────────────────────────────────────
static void test_tiers() {
    TEST("three tiers (SMALL/MEDIUM/LARGE)");

    auto& pool = MemoryPool::instance();

    void* s = pool.allocate(MemoryPool::Tier::SMALL);
    void* m = pool.allocate(MemoryPool::Tier::MEDIUM);
    void* l = pool.allocate(MemoryPool::Tier::LARGE);

    ASSERT(s != m && m != l, "tier pointers should differ");
    ASSERT(s != nullptr && m != nullptr && l != nullptr, "all tiers must allocate");

    pool.deallocate(s, MemoryPool::Tier::SMALL);
    pool.deallocate(m, MemoryPool::Tier::MEDIUM);
    pool.deallocate(l, MemoryPool::Tier::LARGE);

    PASS();
}

// ── 测试4：多线程并发 ──────────────────────────────────
static void test_concurrent() {
    TEST("concurrent thread-local pool");

    auto& pool = MemoryPool::instance();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, &errors]() {
            try {
                for (int i = 0; i < kPerThread; ++i) {
                    for (int tier = 0; tier < 3; ++tier) {
                        void* p = pool.allocate(static_cast<MemoryPool::Tier>(tier));
                        if (!p) { errors.fetch_add(1); continue; }
                        // 写入+读取验证
                        *static_cast<uint8_t*>(p) = 0xAA;
                        pool.deallocate(p, static_cast<MemoryPool::Tier>(tier));
                    }
                }
            } catch (...) {
                errors.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) t.join();

    ASSERT(errors.load() == 0, "concurrent allocation errors");
    PASS();
}

// ── 模块入口 ─────────────────────────────────────────────
namespace wemeet_test {

int test_memory_pool() {
    printf("\n  === MemoryPool Unit Tests ===\n\n");

    tests_passed = 0;
    tests_failed = 0;

    test_basic_alloc();
    test_pool_hit();
    test_tiers();
    test_concurrent();

    printf("\n  MemoryPool: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
