#include "event_loop_pool.h"
#include "logger.h"

namespace wemeet {

EventLoopPool::EventLoopPool(size_t num_loops) {
    if (num_loops == 0) {
        num_loops = std::thread::hardware_concurrency();
        if (num_loops == 0) num_loops = 4;
    }

    main_loop_ = std::make_unique<EventLoop>();

    loops_.reserve(num_loops);
    for (size_t i = 0; i < num_loops; ++i) {
        loops_.push_back(std::make_unique<EventLoop>());
    }

    LOG_INFO("EventLoopPool created: main + %zu worker loops", num_loops);
}

EventLoopPool::~EventLoopPool() {
    stop();
}

void EventLoopPool::start() {
    for (size_t i = 0; i < loops_.size(); ++i) {
        threads_.emplace_back([this, i]() {
            loops_[i]->loop();
        });
    }
    LOG_INFO("EventLoopPool started: %zu worker threads", threads_.size());
}

void EventLoopPool::stop() {
    for (auto& loop : loops_) {
        loop->quit();
    }
    if (main_loop_) {
        main_loop_->quit();
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    LOG_INFO("EventLoopPool stopped");
}

EventLoop* EventLoopPool::next_loop() {
    // 轮询分发（Round-Robin）
    size_t idx = next_index_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[idx].get();
}

} // namespace wemeet
