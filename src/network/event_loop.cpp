#include "event_loop.h"
#include "logger.h"
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <cassert>

namespace wemeet {

EventLoop::EventLoop()
    : events_(kMaxEvents) {

    // 创建 epoll 实例
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        LOG_FATAL("epoll_create1 failed: %s", strerror(errno));
    }

    // 创建 eventfd 用于唤醒
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        LOG_FATAL("eventfd failed: %s", strerror(errno));
    }

    // 注册 wakeup fd
    // 注意：此时 loop 尚未运行（running_ == false），add_read_event 走直接路径
    add_read_event(wakeup_fd_, [this]() {
        uint64_t val;
        ::read(wakeup_fd_, &val, sizeof(val));
    });

    LOG_INFO("EventLoop created: epoll_fd=%d, wakeup_fd=%d", epoll_fd_, wakeup_fd_);
}

EventLoop::~EventLoop() {
    quit();
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    LOG_DEBUG("EventLoop destroyed");
}

// ── 事件循环主循环 ──────────────────────────────────────
void EventLoop::loop() {
    assert(!is_running());
    running_.store(true, std::memory_order_release);
    thread_id_ = std::this_thread::get_id();
    quit_.store(false, std::memory_order_release);

    LOG_INFO("EventLoop starting...");

    while (!quit_.load(std::memory_order_acquire)) {
        // epoll_wait 边缘触发模式
        int ready = ::epoll_wait(epoll_fd_, events_.data(), kMaxEvents, kEpollTimeoutMs);

        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait error: %s", strerror(errno));
            break;
        }

        // 处理就绪事件（仅 loop 线程：fd 回调表/定时器表均无锁访问，#6）
        for (int i = 0; i < ready; ++i) {
            int fd = events_[i].data.fd;
            uint32_t ev = events_[i].events;

            if (fd == wakeup_fd_) {
                // wakeup — 消费 eventfd 数据
                uint64_t val;
                ::read(wakeup_fd_, &val, sizeof(val));
                process_pending_tasks();
                continue;
            }

            // 错误/挂起
            if (ev & (EPOLLERR | EPOLLHUP)) {
                LOG_WARN("fd=%d EPOLLERR/EPOLLHUP", fd);
                if (timers_.count(fd)) {
                    handle_timer(fd);
                }
                continue;
            }

            // 定时器事件
            if (timers_.count(fd)) {
                handle_timer(fd);
            }

            // 可读事件（无锁读回调表 — 仅本线程访问）
            if (ev & EPOLLIN) {
                auto it = fd_read_callbacks_.find(fd);
                if (it != fd_read_callbacks_.end() && it->second) {
                    it->second();
                }
            }

            // 可写事件（同表回调）
            if (ev & EPOLLOUT) {
                auto it = fd_read_callbacks_.find(fd);
                if (it != fd_read_callbacks_.end() && it->second) {
                    it->second();
                }
            }
        }
    }

    running_.store(false, std::memory_order_release);
    LOG_INFO("EventLoop stopped");
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    wakeup();
}

// ── IO 事件管理 ──────────────────────────────────────────
// 线程模型（#6）：loop 运行中且非 loop 线程 → run_in_loop 投递；
// loop 线程内或 loop 未运行（构造期注册）→ 直接执行。

void EventLoop::add_read_event(int fd, ReadCallback cb) {
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, fd, cb = std::move(cb)]() mutable {
            add_read_event(fd, std::move(cb));
        });
        return;
    }
    fd_read_callbacks_[fd] = std::move(cb);
    update_channel(fd, EPOLLIN | EPOLLET);
}

void EventLoop::enable_write(int fd) {
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, fd]() { enable_write(fd); });
        return;
    }
    update_channel(fd, EPOLLIN | EPOLLOUT | EPOLLET);
}

void EventLoop::disable_write(int fd) {
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, fd]() { disable_write(fd); });
        return;
    }
    update_channel(fd, EPOLLIN | EPOLLET);
}

void EventLoop::remove_fd(int fd) {
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, fd]() { remove_fd(fd); });
        return;
    }
    fd_read_callbacks_.erase(fd);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void EventLoop::update_channel(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    // 尝试 MOD，失败则 ADD
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        if (errno == ENOENT) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        }
    }
}

// ── 定时器 ───────────────────────────────────────────────
int EventLoop::run_after(int64_t delay_ms, TimerCallback cb) {
    // timer_id 在调用线程先行分配（原子），保证跨线程调用也能同步返回 id
    int id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, id, delay_ms, cb = std::move(cb)]() mutable {
            create_timer(id, delay_ms, false, std::move(cb));
        });
        return id;
    }
    return create_timer(id, delay_ms, false, std::move(cb));
}

int EventLoop::run_every(int64_t interval_ms, TimerCallback cb) {
    int id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, id, interval_ms, cb = std::move(cb)]() mutable {
            create_timer(id, interval_ms, true, std::move(cb));
        });
        return id;
    }
    return create_timer(id, interval_ms, true, std::move(cb));
}

/**
 * @brief 定时器创建（仅 loop 线程 / loop 未运行时）
 *
 * timerfd_create + timerfd_settime + 注册 epoll，同时维护
 * timers_（fd → entry）与 timer_id_to_fd_（id → fd）两张表。
 */
int EventLoop::create_timer(int id, int64_t interval_ms, bool repeat, TimerCallback cb) {
    int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) return -1;

    struct itimerspec its{};
    its.it_value.tv_sec  = interval_ms / 1000;
    its.it_value.tv_nsec = (interval_ms % 1000) * 1000000;
    if (repeat) {
        its.it_interval.tv_sec  = interval_ms / 1000;
        its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
    }

    ::timerfd_settime(timer_fd, 0, &its, nullptr);

    auto entry = std::make_unique<TimerEntry>();
    entry->id          = id;
    entry->fd          = timer_fd;
    entry->callback    = std::move(cb);
    entry->repeat      = repeat;
    entry->interval_ms = repeat ? interval_ms : 0;

    timers_[timer_fd] = std::move(entry);
    timer_id_to_fd_[id] = timer_fd;   // #8：id → fd 映射，cancel_timer 用

    // 注册到 epoll
    update_channel(timer_fd, EPOLLIN | EPOLLET);

    return id;
}

/**
 * @brief 取消定时器（#8 完整实现）
 *
 * 在 loop 线程内执行（跨线程自动投递）：
 *  1. timer_id_to_fd_ 查到 timerfd；
 *  2. epoll_ctl DEL 摘除监听；
 *  3. close(fd) 释放 timerfd（不泄漏）；
 *  4. 清除 timers_ 与 timer_id_to_fd_ 两表 —— 回调不再触发。
 */
void EventLoop::cancel_timer(int timer_id) {
    if (is_running() && !is_in_loop_thread()) {
        run_in_loop([this, timer_id]() { cancel_timer(timer_id); });
        return;
    }
    auto id_it = timer_id_to_fd_.find(timer_id);
    if (id_it == timer_id_to_fd_.end()) return;   // 不存在/已取消：幂等

    int timer_fd = id_it->second;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, timer_fd, nullptr);
    ::close(timer_fd);
    timers_.erase(timer_fd);
    timer_id_to_fd_.erase(id_it);
}

void EventLoop::handle_timer(int timer_fd) {
    // 读取 timerfd（消费事件）
    uint64_t expirations;
    ::read(timer_fd, &expirations, sizeof(expirations));

    auto it = timers_.find(timer_fd);
    if (it == timers_.end()) return;

    // 执行回调
    if (it->second->callback) {
        it->second->callback();
    }

    // 一次性定时器 → 清理（同步清除 id 映射，#8）
    if (!it->second->repeat) {
        timer_id_to_fd_.erase(it->second->id);
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, timer_fd, nullptr);
        ::close(timer_fd);
        timers_.erase(it);
    }
}

// ── 线程安全投递 ─────────────────────────────────────────
void EventLoop::run_in_loop(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    wakeup();
}

void EventLoop::wakeup() {
    uint64_t val = 1;
    ::write(wakeup_fd_, &val, sizeof(val));
}

void EventLoop::process_pending_tasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        tasks.swap(pending_tasks_);
    }
    for (auto& task : tasks) {
        task();
    }
}

} // namespace wemeet
