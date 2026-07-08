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

// ── ThreadLocalPool 实现 ─────────────────────────────────
void* MemoryPool::ThreadLocalPool::allocate(
        Tier tier, size_t block_size, std::atomic<uint64_t>& hits) {

    auto& freelist = free_lists[static_cast<size_t>(tier)];

    // 优先从自由链表获取 → 池命中
    if (!freelist.empty()) {
        void* ptr = freelist.back();
        freelist.pop_back();
        hits.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }

    // 池未命中 → 调用 malloc
    return std::aligned_alloc(64, block_size);   // 64 字节对齐，利于 SIMD
}

void MemoryPool::ThreadLocalPool::deallocate(
        void* ptr, Tier tier, size_t /*block_size*/) {

    auto& freelist = free_lists[static_cast<size_t>(tier)];

    // 限制池大小，超出则归还系统
    if (freelist.size() < MemoryPool::kMaxBlocksPerTier) {
        freelist.push_back(ptr);
    } else {
        std::free(ptr);
    }
}

MemoryPool::ThreadLocalPool::~ThreadLocalPool() {
    // 线程退出时归还所有内存块
    for (size_t t = 0; t < static_cast<size_t>(Tier::COUNT); ++t) {
        for (void* ptr : free_lists[t]) {
            std::free(ptr);
        }
        free_lists[t].clear();
    }
}

} // namespace wemeet
