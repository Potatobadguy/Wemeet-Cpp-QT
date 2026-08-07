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
 *
 * ─────────────────────────────────────────────────────────────
 *  线程模型（单线程 Reactor，#6）
 * ─────────────────────────────────────────────────────────────
 *  fd 回调表（fd_read_callbacks_）与定时器表（timers_ /
 *  timer_id_to_fd_）【仅 loop 线程访问，零锁】。
 *
 *  所有公共方法（add_read_event / remove_fd / enable_write /
 *  disable_write / run_after / run_every / cancel_timer）入口检查
 *  is_in_loop_thread()：
 *    - 在 loop 线程（或 loop 尚未运行，如构造期注册 wakeup fd）：
 *      直接执行；
 *    - 跨线程：自动经 run_in_loop() 投递到 loop 线程执行后返回。
 *
 *  因此事件分发热路径（loop() 读回调表）完全无锁。
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

    // ── IO 事件管理（跨线程调用自动 run_in_loop 投递）─────
    // 注册读事件（EPOLLIN | EPOLLET）
    void add_read_event(int fd, ReadCallback cb);

    // 修改事件
    void enable_write(int fd);
    void disable_write(int fd);

    // 移除 fd 的所有监听
    void remove_fd(int fd);

    // ── 定时器（跨线程调用自动 run_in_loop 投递）──────────
    // 在 delay_ms 毫秒后执行回调（一次性）
    int run_after(int64_t delay_ms, TimerCallback cb);

    // 周期性定时器（返回 timer_id，可用于取消）
    int run_every(int64_t interval_ms, TimerCallback cb);

    /**
     * @brief 取消定时器（#8）
     *
     * 经 timer_id_to_fd_ 定位 timerfd，在 loop 线程内执行
     * epoll_ctl DEL → close(fd) → 清除 timers_ / timer_id_to_fd_，
     * 保证取消后回调不再触发且 fd 不泄漏。
     */
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

    /**
     * @brief 定时器创建内部实现（仅 loop 线程 / loop 未运行时调用）
     * @param id 调用方已分配好的 timer_id（跨线程投递时先行分配）
     */
    int create_timer(int id, int64_t interval_ms, bool repeat, TimerCallback cb);

    struct TimerEntry {
        int         id;             // 对外暴露的 timer_id（cancel_timer 用）
        int         fd;             // timerfd
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

    // 定时器（以下两表仅 loop 线程访问，无锁；next_timer_id_ 可跨线程分配）
    std::atomic<int>            next_timer_id_{1};
    std::map<int, std::unique_ptr<TimerEntry>> timers_;   // fd → TimerEntry
    std::map<int, int>          timer_id_to_fd_;          // timer_id → fd（#8）

    // fd → 读事件回调（仅 loop 线程访问，无锁，#6 删除 fd_callbacks_mutex_）
    std::map<int, ReadCallback>  fd_read_callbacks_;

    // epoll_event 预分配
    std::vector<epoll_event>    events_;
};

} // namespace wemeet
