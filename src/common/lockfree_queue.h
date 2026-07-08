#pragma once
#include <atomic>
#include <cstddef>
#include <vector>
#include <cassert>

namespace wemeet {

/**
 * @brief 无锁 SPSC（单生产者-单消费者）定长环形队列
 *
 * 技术亮点：
 *   - 纯 std::atomic + CAS 实现，无互斥锁
 *   - 正确使用内存序：acquire-release 保证 happens-before
 *   - 适用场景：音视频帧采集线程 → 编码线程单向传递
 *
 * 参考：Dmitry Vyukov 的经典 SPSC 实现
 */
template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity)
        : capacity_(next_power_of_two(capacity)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {
        assert(capacity >= 2 && "capacity must be >= 2");
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    // ── 生产者接口 ───────────────────────────────────────
    // 尝试入队，成功返回 true，队列满返回 false
    bool try_push(const T& item) {
        size_t write = write_pos_.load(std::memory_order_relaxed);
        size_t next  = write + 1;

        if (next - read_pos_.load(std::memory_order_acquire) >= capacity_)
            return false;   // 队列满 (capacity-1 个可用槽)

        buffer_[write & mask_] = item;

        // release: 确保 item 写入对消费者可见
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // 入队（自旋等待，生产者独占，不会死锁）
    void push(const T& item) {
        while (!try_push(item)) {
            // 自旋等待消费者消费
            // 生产环境中可加入 _mm_pause() 降低 CPU 功耗
        }
    }

    // ── 消费者接口 ───────────────────────────────────────
    // 尝试出队，成功返回 true
    bool try_pop(T& item) {
        size_t read = read_pos_.load(std::memory_order_relaxed);

        if (read == write_pos_.load(std::memory_order_acquire))
            return false;   // 队列空

        item = buffer_[read & mask_];

        // release: 确保消费者读取完成后槽位可被生产者重用
        read_pos_.store(read + 1, std::memory_order_release);
        return true;
    }

    // 出队（自旋等待）
    void pop(T& item) {
        while (!try_pop(item)) {
            // 自旋等待生产者写入
        }
    }

    // ── 容量查询 ─────────────────────────────────────────
    bool empty() const {
        return read_pos_.load(std::memory_order_acquire) ==
               write_pos_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return capacity_; }

    // 近似大小（瞬时快照，非原子）
    size_t size_approx() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (capacity_ - r + w);
    }

private:
    static size_t next_power_of_two(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    const size_t         capacity_;
    const size_t         mask_;
    std::vector<T>       buffer_;

    // 缓存行对齐，避免伪共享
    alignas(64) std::atomic<size_t> write_pos_;
    alignas(64) std::atomic<size_t> read_pos_;
};

} // namespace wemeet
