#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

namespace wemeet {

/**
 * @brief 固定大小线程池 — 展示 std::thread / mutex / condition_variable / future
 *
 * 技术亮点：
 *   - std::packaged_task + std::future 支持异步返回值
 *   - 条件变量实现生产者-消费者模型
 *   - 优雅关闭：join 所有工作线程
 *   - 使用 std::function<void()> 配合 lambda 零开销抽象
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    // 禁止拷贝/移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // ── 提交任务 ─────────────────────────────────────────
    // 方式1: 无返回值任务
    template <typename F, typename... Args>
    void enqueue(F&& f, Args&&... args) {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            tasks_.emplace(std::move(task));
        }
        cv_.notify_one();
    }

    // 方式2: 有返回值任务 → future
    template <typename F, typename... Args>
    auto enqueue_with_result(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>     // C++17 返回类型推导
    {
        using result_type = typename std::invoke_result_t<F, Args...>;

        // 使用 packaged_task + shared_ptr 绕过拷贝限制
        auto task = std::make_shared<std::packaged_task<result_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<result_type> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool is stopping");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

    // ── 状态 ─────────────────────────────────────────────
    size_t thread_count() const { return workers_.size(); }
    size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    void worker_loop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
};

} // namespace wemeet
