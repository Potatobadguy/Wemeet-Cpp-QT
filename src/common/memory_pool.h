#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <mutex>
#include <memory>
#include <atomic>
#include <new>

namespace wemeet {

/**
 * @brief 三级固定大小内存池 — 消除高频分配碎片，减少 malloc/free 系统调用
 *
 * 技术亮点：
 *   - 三级分档：Small(8KB) / Medium(64KB) / Large(1MB)
 *   - 线程局部存储（TLS）：每线程独立内存池，零锁竞争
 *   - RAII 回收：线程退出时自动归还所有内存块
 *   - std::atomic 计数器跟踪分配统计
 *
 * 用法：
 *   auto* buf = MemoryPool::instance().allocate(MemoryPool::Tier::SMALL);
 *   // ... 使用 buf ...
 *   MemoryPool::instance().deallocate(buf, MemoryPool::Tier::SMALL);
 */
class MemoryPool {
public:
    enum class Tier : uint8_t {
        SMALL  = 0,   // 8 KB  — 网络包头
        MEDIUM = 1,   // 64 KB — 业务消息体
        LARGE  = 2,   // 1 MB  — 媒体元数据
        COUNT  = 3
    };

    static constexpr size_t kBlockSizes[] = {
        8  * 1024,     // SMALL
        64 * 1024,     // MEDIUM
        1  * 1024 * 1024  // LARGE (1MB)
    };

    static constexpr size_t kMaxBlocksPerTier = 64;

    // ── 单例 ─────────────────────────────────────────────
    static MemoryPool& instance() {
        static MemoryPool pool;
        return pool;
    }

    // ── 分配 / 释放 ──────────────────────────────────────
    void* allocate(Tier tier);
    void  deallocate(void* ptr, Tier tier);

    // ── 统计（原子读取，无锁）───────────────────────────
    struct Stats {
        uint64_t total_allocs   = 0;
        uint64_t total_frees    = 0;
        uint64_t pool_hits      = 0;   // 池命中次数（未触发 malloc）
    };

    Stats stats() const {
        Stats s;
        s.total_allocs = total_allocs_.load(std::memory_order_relaxed);
        s.total_frees  = total_frees_.load(std::memory_order_relaxed);
        s.pool_hits    = pool_hits_.load(std::memory_order_relaxed);
        return s;
    }

    ~MemoryPool();

private:
    MemoryPool();

    // ── 线程局部池 ───────────────────────────────────────
    struct ThreadLocalPool {
        /**
         * 侵入式 freelist（#15）：
         * 空闲块自身首部直接存放下一个空闲块的指针，不再使用
         * std::vector<void*> 外置容器 —— 零额外堆分配、入/出链表 O(1)、
         * 无 vector 扩容/收缩开销。块最小 8KB，足以容纳一个指针。
         */
        struct FreeNode {
            FreeNode* next;
        };

        // 每个 Tier 一个侵入式空闲链表的表头 + 当前块数
        std::array<FreeNode*, static_cast<size_t>(Tier::COUNT)> free_heads{};
        std::array<size_t,    static_cast<size_t>(Tier::COUNT)> free_counts{};

        ~ThreadLocalPool();

        void* allocate(Tier tier, size_t block_size, std::atomic<uint64_t>& pool_hits);
        void  deallocate(void* ptr, Tier tier, size_t block_size);
    };

    static thread_local ThreadLocalPool tls_pool_;

    std::atomic<uint64_t> total_allocs_{0};
    std::atomic<uint64_t> total_frees_{0};
    std::atomic<uint64_t> pool_hits_{0};    // 池命中
};

} // namespace wemeet
