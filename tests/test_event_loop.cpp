/**
 * @brief 单元测试 — EventLoop epoll 事件循环
 */
#include "event_loop.h"
#include "event_loop_pool.h"
#include <cstdio>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>

using namespace wemeet;

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ─��� 测试1：基本创建和退出 ───────────────────────────────
static void test_create_quit() {
    TEST("create and quit");

    EventLoop loop;
    ASSERT(loop.is_running() == false, "should not be running initially");

    // 在另一个线程启动 loop，等待后退出
    std::thread t([&loop]() {
        loop.loop();
    });

    // 等待 loop 启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT(loop.is_running(), "should be running after loop()");

    loop.quit();
    t.join();

    ASSERT(!loop.is_running(), "should not be running after quit");
    PASS();
}

// ── 测试2：定时器 ────────────────────────────────────────
static void test_timer() {
    TEST("timer callback (run_after)");

    EventLoop loop;
    std::atomic<int> counter{0};

    std::thread t([&]() {
        loop.run_after(100, [&]() {
            counter.fetch_add(1);
        });
        loop.run_after(200, [&]() {
            counter.fetch_add(1);
            loop.quit();
        });
        loop.loop();
    });

    t.join();

    ASSERT(counter.load() == 2, "both timers should have fired");
    PASS();
}

// ── 测试3：run_in_loop 跨线程投递 ───────────────────────
static void test_run_in_loop() {
    TEST("run_in_loop (cross-thread task)");

    EventLoop loop;
    std::atomic<int> value{0};

    std::thread t([&]() {
        loop.loop();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 从主线程投递任务
    loop.run_in_loop([&]() {
        value.store(42);
        loop.quit();
    });

    t.join();

    ASSERT(value.load() == 42, "task should have executed in loop thread");
    PASS();
}

// ── 测试4：cancel_timer — 取消后回调不再触发、timerfd 不泄漏（#8）──
static void test_cancel_timer() {
    TEST("cancel_timer (one-shot cancelled before fire)");

    EventLoop loop;
    std::atomic<int> fired{0};

    std::thread t([&]() {
        loop.loop();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 跨线程注册 + 立即取消（验证跨线程 run_in_loop 投递路径）
    int id = loop.run_after(100, [&]() { fired.fetch_add(1); });
    ASSERT(id > 0, "run_after should return a valid timer id");
    loop.cancel_timer(id);

    // 等待超过原定触发时间，回调不应执行
    loop.run_after(300, [&]() { loop.quit(); });
    t.join();

    ASSERT(fired.load() == 0, "cancelled timer must not fire");
    PASS();
}

// ── 测试5：cancel_timer 周期定时器中途取消 ───────────────
static void test_cancel_periodic_timer() {
    TEST("cancel_timer (periodic stopped mid-run)");

    EventLoop loop;
    std::atomic<int> count{0};

    std::thread t([&]() {
        loop.loop();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int id = loop.run_every(50, [&]() { count.fetch_add(1); });
    ASSERT(id > 0, "run_every should return a valid timer id");

    // 让其触发若干次后取消
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    loop.cancel_timer(id);
    int snapshot = count.load();
    ASSERT(snapshot >= 2, "periodic timer should have fired before cancel");

    // 取消后等待多个周期，计数不得再增长
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT(count.load() == snapshot, "no more fires after cancel");

    // 重复取消应为幂等（不崩溃）
    loop.cancel_timer(id);

    loop.quit();
    t.join();
    PASS();
}

// ── 测试6：EventLoopPool ─────────────────────────────────
static void test_pool() {
    TEST("EventLoopPool (One Loop Per Thread)");

    EventLoopPool pool(2);
    pool.start();

    // 等待所有线程完全启动
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT(pool.size() == 2, "pool should have 2 loops");

    // 验证轮询分发
    EventLoop* l1 = pool.next_loop();
    EventLoop* l2 = pool.next_loop();
    EventLoop* l3 = pool.next_loop();

    ASSERT(l1 != nullptr && l2 != nullptr, "loops should not be null");
    // 第三次应该回到第一个（轮询）
    ASSERT(l3 == l1, "third next_loop should wrap around to first");

    pool.stop();
    PASS();
}

// ── 模块入口 ─────────────────────────────────────────────
namespace wemeet_test {

int test_event_loop() {
    printf("\n  === EventLoop / Reactor Unit Tests ===\n\n");

    tests_passed = tests_failed = 0;

    test_create_quit();
    test_timer();
    test_run_in_loop();
    test_cancel_timer();
    test_cancel_periodic_timer();
    test_pool();

    printf("\n  EventLoop: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
