#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <utility>
#include <cassert>

namespace wemeet {

/**
 * @brief 高效字节缓冲区 — 展示移动语义、右值引用
 *
 * 设计要点：
 *   - 移动构造/移动赋值 → 零拷贝所有权转移，避免音视频帧重复拷贝
 *   - RAII 自动管理底层内存
 *   - preppend/reserve 支持协议头预留
 */
class Buffer {
public:
    static constexpr size_t kInitialSize = 1024;
    static constexpr size_t kPrependSize = 8;   // 协议头预留空间

    // ── 构造/析构 ───────────────────────────────────────
    Buffer() : buffer_(kPrependSize + kInitialSize),
               read_idx_(kPrependSize),
               write_idx_(kPrependSize) {}

    explicit Buffer(size_t initial_size)
        : buffer_(kPrependSize + initial_size),
          read_idx_(kPrependSize),
          write_idx_(kPrependSize) {}

    // ── 移动语义 — 零拷贝传递（核心：右值引用）─────────
    Buffer(Buffer&& other) noexcept
        : buffer_(std::move(other.buffer_)),
          read_idx_(other.read_idx_),
          write_idx_(other.write_idx_) {
        other.read_idx_ = kPrependSize;
        other.write_idx_ = kPrependSize;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            buffer_    = std::move(other.buffer_);
            read_idx_  = other.read_idx_;
            write_idx_  = other.write_idx_;
            other.read_idx_ = kPrependSize;
            other.write_idx_ = kPrependSize;
        }
        return *this;
    }

    // 禁止拷贝（音视频帧不应被复制）
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // ── 数据访问 ─────────────────────────────────────────
    const uint8_t* data() const { return buffer_.data() + read_idx_; }
    uint8_t* data()             { return buffer_.data() + read_idx_; }

    size_t readable_size()  const { return write_idx_ - read_idx_; }
    size_t writable_size()  const { return buffer_.size() - write_idx_; }
    size_t prependable_size() const { return read_idx_; }

    const uint8_t* peek() const { return data(); }

    // ── 读写操作 ─────────────────────────────────────────
    void retrieve(size_t len) {
        assert(len <= readable_size());
        if (len < readable_size()) {
            read_idx_ += len;
        } else {
            retrieve_all();
        }
    }

    void retrieve_all() {
        read_idx_  = kPrependSize;
        write_idx_ = kPrependSize;
    }

    std::string retrieve_as_string(size_t len) {
        assert(len <= readable_size());
        std::string result(reinterpret_cast<const char*>(data()), len);
        retrieve(len);
        return result;
    }

    void append(const void* data, size_t len) {
        ensure_writable(len);
        std::memcpy(buffer_.data() + write_idx_, data, len);
        write_idx_ += len;
    }

    void append(const std::string& str) {
        append(str.data(), str.size());
    }

    void prepend(const void* data, size_t len) {
        assert(len <= prependable_size());
        read_idx_ -= len;
        std::memcpy(buffer_.data() + read_idx_, data, len);
    }

    // ── 智能指针风格工厂方法 ────────────────────────────
    static std::unique_ptr<Buffer> create(size_t size = kInitialSize) {
        return std::make_unique<Buffer>(size);
    }

    // 从 vector 高效构造（移动语义）
    static Buffer from_vector(std::vector<uint8_t>&& vec) {
        Buffer buf(0);
        buf.buffer_ = std::move(vec);
        buf.read_idx_ = 0;
        buf.write_idx_ = buf.buffer_.size();
        return buf;
    }

    // 转换为字符串（移动语义，避免拷贝）
    std::string to_string() && {
        return retrieve_as_string(readable_size());
    }

private:
    void ensure_writable(size_t len) {
        if (writable_size() >= len) return;
        // 如果前面有已读空间可以回收
        if (writable_size() + prependable_size() >= len + kPrependSize) {
            size_t readable = readable_size();
            std::memmove(buffer_.data() + kPrependSize,
                         buffer_.data() + read_idx_, readable);
            read_idx_  = kPrependSize;
            write_idx_ = kPrependSize + readable;
        } else {
            buffer_.resize(write_idx_ + len);
        }
    }

    std::vector<uint8_t> buffer_;
    size_t read_idx_;
    size_t write_idx_;
};

/**
 * @brief 缓冲区视图 — 零拷贝切片
 *
 * 不拥有数据，仅持有指针 + 长度。
 * Lambda 捕获使用，避免拷贝。
 */
struct BufferView {
    const uint8_t* data;
    size_t         size;

    BufferView(const uint8_t* d, size_t s) : data(d), size(s) {}
    BufferView(const Buffer& buf) : data(buf.data()), size(buf.readable_size()) {}
};

} // namespace wemeet
