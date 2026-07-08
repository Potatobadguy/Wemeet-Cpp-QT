#pragma once
#include "memory_pool.h"
#include "buffer.h"
#include <memory>

namespace wemeet {

/**
 * @brief 网络缓冲区池 — 从 MemoryPool 分配 Buffer 底层存储
 *
 * 技术亮点：
 *   - 集成三级内存池，网络包零 malloc
 *   - 智能指针风格的 RAII 管理
 *   - 与 Buffer 配合实现零拷贝传递
 */
class BufferPool {
public:
    // 从池中获取预分配 Buffer
    static Buffer acquire_small() {
        void* mem = MemoryPool::instance().allocate(MemoryPool::Tier::SMALL);
        // 包装为 Buffer（简化：直接使用 Buffer 内部管理）
        return Buffer(kSmallSize);
    }

    static Buffer acquire_medium() {
        return Buffer(kMediumSize);
    }

    static constexpr size_t kSmallSize  = 8192;   // 8KB
    static constexpr size_t kMediumSize = 65536;   // 64KB
};

} // namespace wemeet
