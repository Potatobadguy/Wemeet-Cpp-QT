#include "memory_pool.h"
#include <cstdlib>
#include <algorithm>

namespace wemeet {

// ── MemoryPool 单例实现 ──────────────────────────────────

thread_local MemoryPool::ThreadLocalPool MemoryPool::tls_pool_;

MemoryPool::MemoryPool() = default;

MemoryPool::~MemoryPool() {
    // 线程局部池在线程退出时自动析构（tls destructor）
}

// ── 公共分配接口 ─────────────────────────────────────────
void* MemoryPool::allocate(Tier tier) {
    total_allocs_.fetch_add(1, std::memory_order_relaxed);

    size_t block_size = kBlockSizes[static_cast<size_t>(tier)];
    return tls_pool_.allocate(tier, block_size, pool_hits_);
}

void MemoryPool::deallocate(void* ptr, Tier tier) {
    total_frees_.fetch_add(1, std::memory_order_relaxed);

    if (!ptr) return;
    size_t block_size = kBlockSizes[static_cast<size_t>(tier)];
    tls_pool_.deallocate(ptr, tier, block_size);
}

// ── ThreadLocalPool 实现（侵入式 freelist）─────────────────
void* MemoryPool::ThreadLocalPool::allocate(
        Tier tier, size_t block_size, std::atomic<uint64_t>& hits) {

    const size_t idx = static_cast<size_t>(tier);

    // 优先从侵入式空闲链表摘取头节点 → 池命中（O(1)，无锁）
    if (FreeNode* node = free_heads[idx]) {
        free_heads[idx] = node->next;
        --free_counts[idx];
        hits.fetch_add(1, std::memory_order_relaxed);
        return static_cast<void*>(node);
    }

    // 池未命中 → 调用 malloc
    return std::aligned_alloc(64, block_size);   // 64 字节对齐，利于 SIMD
}

void MemoryPool::ThreadLocalPool::deallocate(
        void* ptr, Tier tier, size_t /*block_size*/) {

    const size_t idx = static_cast<size_t>(tier);

    // 限制池大小，超出则归还系统
    if (free_counts[idx] < MemoryPool::kMaxBlocksPerTier) {
        // 空闲块首部写入 next 指针（侵入式链接）
        auto* node = static_cast<FreeNode*>(ptr);
        node->next = free_heads[idx];
        free_heads[idx] = node;
        ++free_counts[idx];
    } else {
        std::free(ptr);
    }
}

MemoryPool::ThreadLocalPool::~ThreadLocalPool() {
    // 线程退出时遍历侵入式链表，归还所有内存块
    for (size_t t = 0; t < static_cast<size_t>(Tier::COUNT); ++t) {
        FreeNode* node = free_heads[t];
        while (node) {
            FreeNode* next = node->next;
            std::free(static_cast<void*>(node));
            node = next;
        }
        free_heads[t]  = nullptr;
        free_counts[t] = 0;
    }
}

} // namespace wemeet
