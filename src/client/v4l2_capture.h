#pragma once
#include <QObject>
#include <QImage>
#include <QTimer>
#include <QVideoFrame>
#include <QMutex>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

/**
 * @brief V4L2 摄像头捕获 — 直接通过 Linux V4L2 API 采集摄像头帧
 *
 * 当 Qt Multimedia 的 QCamera 不可用时（如 WSL 环境），
 * 降级使用 V4L2 直接读取 /dev/video* 设备。
 */
class V4L2Capture : public QObject {
    Q_OBJECT
public:
    explicit V4L2Capture(QObject* parent = nullptr);
    ~V4L2Capture();

    // ── 生命周期 ─────────────────────────────────────────
    bool open(const std::string& device_path = "/dev/video0",
              int width = 640, int height = 480, int fps = 30);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // ── 控制 ─────────────────────────────────────────────
    bool start();
    void stop();
    bool is_running() const { return running_; }

    // ── 设备信息 ─────────────────────────────────────────
    std::string device_name() const { return device_name_; }
    int fd() const { return fd_; }

    // ── 静态检测 ─────────────────────────────────────────
    static bool has_v4l2_device(const std::string& device_path = "/dev/video0");
    static int  enum_devices(std::vector<std::string>& devices);

signals:
    void frame_captured(const QVideoFrame& frame);
    void error_occurred(const std::string& error);

private slots:
    void capture_frame();

private:
    bool init_device(int width, int height);
    bool init_mmap();
    bool start_stream();
    bool stop_stream();
    bool try_read_mode();       // read() 方式降级

    // 缓冲区
    struct Buffer {
        void*   start = nullptr;
        size_t  length = 0;
    };

    int  fd_ = -1;
    std::string device_name_;

    // 流状态
    std::atomic<bool> running_{false};
    bool use_read_mode_ = false;  // 使用 read() 而非 mmap

    // MMAP 缓冲区
    std::vector<Buffer> buffers_;
    static constexpr int kBufferCount = 4;

    // 格式
    int width_  = 640;
    int height_ = 480;
    int fps_    = 30;

    // 捕获定时器
    QTimer* capture_timer_ = nullptr;

    // 帧计数
    int64_t frame_count_ = 0;
};
