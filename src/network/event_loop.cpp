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

        // 处理就绪事件
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

            // 可读事件
            if (ev & EPOLLIN) {
                // 回调由外部注册 — 这里只处理定时器 fd 和 wakeup_fd
                // 普通 fd 的回调在外部 tcp_connection 中处理
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
void EventLoop::add_read_event(int fd, ReadCallback /*cb*/) {
    update_channel(fd, EPOLLIN | EPOLLET);
    // 回调通过外部存储管理（tcp_connection 持有）
}

void EventLoop::enable_write(int fd) {
    update_channel(fd, EPOLLIN | EPOLLOUT | EPOLLET);
}

void EventLoop::disable_write(int fd) {
    update_channel(fd, EPOLLIN | EPOLLET);
}

void EventLoop::remove_fd(int fd) {
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
    int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) return -1;

    // 设置定时器参数
    struct itimerspec its{};
    its.it_value.tv_sec  = delay_ms / 1000;
    its.it_value.tv_nsec = (delay_ms % 1000) * 1000000;
    // it_interval 为 0 → 一次性定时器

    ::timerfd_settime(timer_fd, 0, &its, nullptr);

    int id = next_timer_id_++;
    auto entry = std::make_unique<TimerEntry>();
    entry->fd       = timer_fd;
    entry->callback = std::move(cb);
    entry->repeat   = false;
    entry->interval_ms = 0;

    timers_[timer_fd] = std::move(entry);

    // 注册到 epoll
    update_channel(timer_fd, EPOLLIN | EPOLLET);

    return id;
}

int EventLoop::run_every(int64_t interval_ms, TimerCallback cb) {
    int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) return -1;

    struct itimerspec its{};
    its.it_value.tv_sec     = interval_ms / 1000;
    its.it_value.tv_nsec    = (interval_ms % 1000) * 1000000;
    its.it_interval.tv_sec  = interval_ms / 1000;
    its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;

    ::timerfd_settime(timer_fd, 0, &its, nullptr);

    int id = next_timer_id_++;
    auto entry = std::make_unique<TimerEntry>();
    entry->fd       = timer_fd;
    entry->callback = std::move(cb);
    entry->repeat   = true;
    entry->interval_ms = interval_ms;

    timers_[timer_fd] = std::move(entry);
    update_channel(timer_fd, EPOLLIN | EPOLLET);

    return id;
}

void EventLoop::cancel_timer(int /*timer_id*/) {
    // 简化实现：暂不按 id 取消
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

    // 一次性定时器 → 清理
    if (!it->second->repeat) {
        remove_fd(timer_fd);
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
