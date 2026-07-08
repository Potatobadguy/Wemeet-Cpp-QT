#include "thread_pool.h"
#include <iostream>

namespace wemeet {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) num_threads = 4;
    workers_.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 条件变量等待：避免忙等 CPU 空转
            cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !tasks_.empty();
            });

            if (stop_.load(std::memory_order_acquire) && tasks_.empty()) {
                return;   // 优雅退出
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 在锁外执行任务，提高并发度
        task();
    }
}

} // namespace wemeet
