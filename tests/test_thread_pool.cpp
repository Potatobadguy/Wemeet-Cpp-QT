/**
 * @brief 单元测试 — 线程池
 */

#include "thread_pool.h"
#include <cstdio>
#include <cassert>
#include <atomic>
#include <chrono>
#include <vector>

using namespace wemeet;

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ── 测试1：基础任务提交 ────────────────────────────────
static void test_basic() {
    TEST("basic task submission");

    ThreadPool pool(4);
    std::atomic<int> counter{0};

    for (int i = 0; i < 100; ++i) {
        pool.enqueue([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // 等待任务完成（pool 析构会 join）
    // 手动画一个短暂的等待确认
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT(counter.load() == 100, "not all tasks executed");
    PASS();
}

// ── 测试2：有返回值任务 ────────────────────────────────
static void test_future() {
    TEST("task with return value (std::future)");

    ThreadPool pool(2);

    auto future = pool.enqueue_with_result([](int a, int b) -> int {
        return a + b;
    }, 10, 20);

    int result = future.get();
    ASSERT(result == 30, "future result mismatch");
    PASS();
}

// ── 测试3：并发执行验证 ────────────────────────────────
static void test_concurrent() {
    TEST("concurrent execution");

    ThreadPool pool(8);
    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 32; ++i) {
        futures.push_back(pool.enqueue_with_result([&]() {
            int curr = running.fetch_add(1) + 1;
            // 更新最大并发数
            int prev = max_concurrent.load();
            while (curr > prev &&
                   !max_concurrent.compare_exchange_weak(prev, curr)) {
                prev = max_concurrent.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            running.fetch_sub(1);
        }));
    }

    for (auto& f : futures) f.get();

    ASSERT(max_concurrent.load() >= 2, "pool should run at least 2 tasks concurrently");
    printf("  [max concurrent: %d]\n", max_concurrent.load());
    PASS();
}

// ── main ─────────────────────────────────────────────────
namespace wemeet_test {

int test_thread_pool() {
    printf("\n  === ThreadPool Unit Tests ===\n\n");

    tests_passed = 0;
    tests_failed = 0;

    test_basic();
    test_future();
    test_concurrent();

    printf("\n  ThreadPool: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
