#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <queue>
#include <atomic>
#include <cstdio>

namespace wemeet {

/**
 * @brief 异步高性能日志系统 — 展示双缓冲 + 条件变量
 *
 * 技术亮点：
 *   - 双缓冲（Double Buffering）：前台写满后交换到后台线程刷盘
 *   - 条件变量：前台等待后台刷盘完成
 *   - 日志级别 + 格式化时间戳
 *   - 线程安全：多线程可同时写入
 */
enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    FATAL = 4
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // ── 配置 ─────────────────────────────────────────────
    void set_level(LogLevel level) { level_ = level; }
    void set_file(const std::string& path);
    void set_console(bool enable);

    // ── 日志接口 ─────────────────────────────────────────
    void log(LogLevel level, const char* file, int line,
             const char* func, const char* fmt, ...);

#define LOG_DEBUG(fmt, ...) Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Logger::instance().log(LogLevel::INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::instance().log(LogLevel::WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) Logger::instance().log(LogLevel::FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

    ~Logger();

private:
    Logger();
    void background_flush();

    static constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4MB 缓冲区

    // 双缓冲
    struct Buffer {
        char   data[kBufferSize];
        size_t used = 0;
    };

    Buffer                  buffers_[2];
    int                     current_buffer_ = 0;        // 当前写入缓冲
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             flush_thread_;
    std::ofstream           file_stream_;
    std::atomic<bool>       running_{true};
    std::atomic<bool>       buffer_ready_{false};       // 后台缓冲是否可用
    LogLevel                level_{LogLevel::DEBUG};
    bool                    console_enabled_ = true;
};

} // namespace wemeet
