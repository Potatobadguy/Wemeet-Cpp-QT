#pragma once
#include "event_loop.h"
#include <vector>
#include <thread>
#include <memory>
#include <atomic>

namespace wemeet {

/**
 * @brief One Loop Per Thread 事件循环池 — Reactor 多线程模型
 *
 * 技术亮点：
 *   - 主 Reactor 负责 accept 新连接
 *   - 子 Reactor 负责 I/O 读写（轮询分发）
 *   - 每个子 Reactor 运行在独立线程
 */
class EventLoopPool {
public:
    explicit EventLoopPool(size_t num_loops = 0);
    ~EventLoopPool();

    // 不可拷贝
    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;

    // 启动所有子 Reactor 线程
    void start();

    // 停止所有线程
    void stop();

    // 获取下一个 EventLoop（轮询）
    EventLoop* next_loop();

    // 获取主 EventLoop（用于 accept）
    EventLoop* main_loop() { return main_loop_.get(); }

    size_t size() const { return loops_.size(); }

private:
    std::unique_ptr<EventLoop>      main_loop_;
    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<std::thread>        threads_;
    std::atomic<size_t>             next_index_{0};
};

} // namespace wemeet
