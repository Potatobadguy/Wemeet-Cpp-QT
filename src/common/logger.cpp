#include "logger.h"
#include <cstdarg>
#include <cstring>
#include <iostream>

namespace wemeet {

Logger::Logger() {
    flush_thread_ = std::thread(&Logger::background_flush, this);
}

Logger::~Logger() {
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (flush_thread_.joinable()) flush_thread_.join();
    if (file_stream_.is_open()) file_stream_.close();
}

void Logger::set_file(const std::string& path) {
    if (file_stream_.is_open()) file_stream_.close();
    file_stream_.open(path, std::ios::app);
    if (!file_stream_) {
        std::cerr << "[Logger] Cannot open log file: " << path << "\n";
    }
}

void Logger::set_console(bool enable) {
    console_enabled_ = enable;
}

// ── 核心日志写入 ─────────────────────────────────────────
void Logger::log(LogLevel level, const char* file, int line,
                 const char* func, const char* fmt, ...) {
    if (level < level_) return;

    // ── 格式化时间 ───────────────────────────────────────
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);

    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // ── 日志级别字符串 ───────────────────────────────────
    static const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    const char* level_name = level_names[static_cast<int>(level)];

    // 提取文件名（去掉路径）
    const char* filename = std::strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    // ── 格式化消息 ───────────────────────────────────────
    char msg[4096];
    va_list args;
    va_start(args, fmt);
    int msg_len = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (msg_len < 0) return;

    // ── 组装完整日志行 ───────────────────────────────────
    char line_buf[5120];
    int total = snprintf(line_buf, sizeof(line_buf),
        "[%s.%03ld] [%s] [%s:%d %s] %s\n",
        time_str, ms.count(), level_name, filename, line, func, msg);

    // ── 双缓冲写入 ───────────────────────────────────────
    {
        std::unique_lock<std::mutex> lock(mutex_);

        Buffer& buf = buffers_[current_buffer_];

        // 如果当前缓冲区快满 → 交换到后台
        if (buf.used + total >= kBufferSize) {
            // 等待后台线程完成上一轮刷盘
            cv_.wait(lock, [this] {
                return !buffer_ready_.load(std::memory_order_acquire);
            });

            // 交换缓冲区
            current_buffer_ = 1 - current_buffer_;
            buffer_ready_.store(true, std::memory_order_release);
            cv_.notify_one();   // 唤醒后台线程

            // 写入新缓冲区
            Buffer& new_buf = buffers_[current_buffer_];
            std::memcpy(new_buf.data + new_buf.used, line_buf, total);
            new_buf.used += total;

        } else {
            std::memcpy(buf.data + buf.used, line_buf, total);
            buf.used += total;
        }
    }

    // ── 控制台输出 ───────────────────────────────────────
    if (console_enabled_) {
        if (level >= LogLevel::ERROR) {
            std::cerr << line_buf;
        } else {
            std::cout << line_buf;
        }
    }

    // FATAL 级别立即刷盘并终止
    if (level == LogLevel::FATAL) {
        std::abort();
    }
}

// ── 后台刷盘线程 ─────────────────────────────────────────
void Logger::background_flush() {
    while (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 等待有数据需要刷盘
        cv_.wait(lock, [this] {
            return buffer_ready_.load(std::memory_order_acquire) ||
                   !running_.load(std::memory_order_acquire);
        });

        if (!running_.load(std::memory_order_acquire)) break;

        // 刷盘前缓冲区（非当前缓冲）
        int flush_idx = 1 - current_buffer_;
        Buffer& flush_buf = buffers_[flush_idx];

        if (flush_buf.used > 0) {
            // 写入文件
            if (file_stream_.is_open()) {
                file_stream_.write(flush_buf.data, flush_buf.used);
                file_stream_.flush();
            }

            // 重置缓冲区
            flush_buf.used = 0;
        }

        buffer_ready_.store(false, std::memory_order_release);
        cv_.notify_one();   // 通知前台缓冲区已可用
    }
}

} // namespace wemeet
