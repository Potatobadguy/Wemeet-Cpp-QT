#pragma once
#include <sys/epoll.h>
#include <unistd.h>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <map>
#include <chrono>
#include <thread>
#include <mutex>

namespace wemeet {

/**
 * @brief epoll 边缘触发（ET）事件循环 — Reactor 核心
 *
 * 技术亮点：
 *   - epoll_create1 + EPOLLET 边缘触发
 *   - timerfd 实现高精度定时器
 *   - 事件回调使用 std::function + lambda 零开销绑定
 *   - 优雅退出机制
 */
class EventLoop {
public:
    using EventCallback  = std::function<void()>;
    using ReadCallback   = std::function<void()>;
    using TimerCallback  = std::function<void()>;

    explicit EventLoop();
    ~EventLoop();

    // 禁止拷贝/移动
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ── 事件循环 ─────────────────────────────────────────
    void loop();        // 启动事件循环（阻塞）
    void quit();        // 优雅退出

    // ── IO 事件管理 ──────────────────────────────────────
    // 注册读事件（EPOLLIN | EPOLLET）
    void add_read_event(int fd, ReadCallback cb);

    // 修改事件
    void enable_write(int fd);
    void disable_write(int fd);

    // 移除 fd 的所有监听
    void remove_fd(int fd);

    // ── 定时器 ───────────────────────────────────────────
    // 在 delay_ms 毫秒后执行回调（一次性）
    int run_after(int64_t delay_ms, TimerCallback cb);

    // 周期性定时器（返回 timer_id，可用于取消）
    int run_every(int64_t interval_ms, TimerCallback cb);

    // 取消定时器
    void cancel_timer(int timer_id);

    // ── 线程安全 ─────────────────────────────────────────
    // 从其他线程投递任务到本 loop
    void run_in_loop(std::function<void()> task);
    void wakeup();  // 唤醒 epoll_wait

    // ── 状态 ─────────────────────────────────────────────
    bool is_in_loop_thread() const {
        return thread_id_ == std::this_thread::get_id();
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    void handle_events(int ready_count);
    void handle_timer(int timer_fd);
    void process_pending_tasks();
    void update_channel(int fd, uint32_t events);

    struct TimerEntry {
        int         fd;
        TimerCallback callback;
        bool        repeat;
        int64_t     interval_ms;
    };

    static constexpr int kMaxEvents = 1024;
    static constexpr int kEpollTimeoutMs = 10000;

    int                         epoll_fd_;
    int                         wakeup_fd_;    // eventfd 用于唤醒
    std::atomic<bool>           running_{false};
    std::atomic<bool>           quit_{false};
    std::thread::id             thread_id_;

    // 待处理任务（线程安全）
    std::vector<std::function<void()>> pending_tasks_;
    std::mutex                  pending_mutex_;

    // 定时器
    int                         next_timer_id_ = 1;
    std::map<int, std::unique_ptr<TimerEntry>> timers_;

    // epoll_event 预分配
    std::vector<epoll_event>    events_;
};

} // namespace wemeet
