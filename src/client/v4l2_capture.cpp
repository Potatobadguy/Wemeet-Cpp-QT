#include "v4l2_capture.h"
#include <QDebug>
#include <QElapsedTimer>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <cstring>
#include <cerrno>
#include <chrono>

// ── 构造/析构 ───────────────────────────────────────────────

V4L2Capture::V4L2Capture(QObject* parent)
    : QObject(parent)
    , capture_timer_(new QTimer(this)) {

    capture_timer_->setSingleShot(true);
    connect(capture_timer_, &QTimer::timeout, this, &V4L2Capture::capture_frame);
}

V4L2Capture::~V4L2Capture() {
    stop();
    close();
}

// ── 静态检测函数 ───────────────────────────────────────────

bool V4L2Capture::has_v4l2_device(const std::string& device_path) {
    int fd = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) return false;

    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    bool is_v4l2 = (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
    // 必须是真正的视频采集设备，排除 metadata / 输出设备
    bool can_capture = is_v4l2 && (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE);
    ::close(fd);
    return can_capture;
}

int V4L2Capture::enum_devices(std::vector<std::string>& devices) {
    devices.clear();
    for (int i = 0; i < 10; ++i) {
        std::string path = "/dev/video" + std::to_string(i);
        if (has_v4l2_device(path)) {
            devices.push_back(path);
        }
    }
    return static_cast<int>(devices.size());
}

// ── 打开设备 ───────────────────────────────────────────────

bool V4L2Capture::open(const std::string& device_path, int width, int height, int fps) {
    if (fd_ >= 0) {
        close();
    }

    width_  = width;
    height_ = height;
    fps_    = fps;

    fd_ = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
        emit error_occurred("Cannot open " + device_path + ": " + strerror(errno));
        return false;
    }

    // 查询设备能力
    struct v4l2_capability cap;
    std::memset(&cap, 0, sizeof(cap));
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        emit error_occurred("Not a V4L2 device: " + device_path);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    device_name_ = reinterpret_cast<const char*>(cap.card);

    // 检查是否支持视频捕获
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        emit error_occurred(device_path + " does not support video capture");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 检查是否支持 streaming I/O
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        qWarning("V4L2: %s no streaming support, trying read() instead", device_path.c_str());
    }

    qDebug("V4L2: opened %s — %s", device_path.c_str(), device_name_.c_str());
    return init_device(width, height);
}

void V4L2Capture::close() {
    if (fd_ < 0) return;

    ::close(fd_);
    fd_ = -1;
    qDebug("V4L2: device closed");
}

// ── 初始化设备格式 ─────────────────────────────────────────

bool V4L2Capture::init_device(int width, int height) {
    // 先列出设备支持的格式
    struct v4l2_fmtdesc fmt_desc;
    std::memset(&fmt_desc, 0, sizeof(fmt_desc));
    fmt_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    qDebug("V4L2: supported formats:");
    while (::ioctl(fd_, VIDIOC_ENUM_FMT, &fmt_desc) == 0) {
        qDebug("  [%d] %c%c%c%c  %s",
               fmt_desc.index,
               (fmt_desc.pixelformat >> 0) & 0xFF,
               (fmt_desc.pixelformat >> 8) & 0xFF,
               (fmt_desc.pixelformat >> 16) & 0xFF,
               (fmt_desc.pixelformat >> 24) & 0xFF,
               fmt_desc.description);
        fmt_desc.index++;
    }

    // 尝试格式列表（按优先级）
    struct FormatEntry {
        uint32_t fourcc;
        const char* name;
    };
    FormatEntry formats[] = {
        {V4L2_PIX_FMT_MJPEG, "MJPEG"},
        {V4L2_PIX_FMT_JPEG,  "JPEG"},
        {V4L2_PIX_FMT_YUYV,  "YUYV"},
    };

    uint32_t selected_fourcc = 0;
    for (auto& f : formats) {
        struct v4l2_format fmt;
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = f.fourcc;
        fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

        if (::ioctl(fd_, VIDIOC_S_FMT, &fmt) == 0) {
            selected_fourcc = fmt.fmt.pix.pixelformat;
            width_  = fmt.fmt.pix.width;
            height_ = fmt.fmt.pix.height;
            qDebug("V4L2: format set to %s (%dx%d)", f.name, width_, height_);
            break;
        }
    }

    if (selected_fourcc == 0) {
        emit error_occurred("Failed to set any video format");
        return false;
    }

    // 设置帧率
    struct v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_G_PARM, &parm) == 0) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps_;
        ::ioctl(fd_, VIDIOC_S_PARM, &parm);
    }

    qDebug("V4L2: format set to %dx%d @ %dfps, fourcc=%c%c%c%c",
           width_, height_, fps_,
           (selected_fourcc >> 0) & 0xFF,
           (selected_fourcc >> 8) & 0xFF,
           (selected_fourcc >> 16) & 0xFF,
           (selected_fourcc >> 24) & 0xFF);

    // 尝试 mmap，失败则降级到 read 模式
    if (init_mmap()) {
        return true;
    }

    qWarning("V4L2: mmap failed, trying read() mode");
    return try_read_mode();
}

// ── 初始化 MMAP 缓冲 ───────────────────────────────────────

bool V4L2Capture::init_mmap() {
    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.count  = kBufferCount;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        emit error_occurred("VIDIOC_REQBUFS failed: " + std::string(strerror(errno)));
        return false;
    }

    if (req.count < 2) {
        emit error_occurred("Insufficient buffer memory");
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (::ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            emit error_occurred("VIDIOC_QUERYBUF failed");
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start = ::mmap(nullptr, buf.length,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd_, buf.m.offset);

        if (buffers_[i].start == MAP_FAILED) {
            emit error_occurred("mmap failed");
            return false;
        }
    }

    qDebug("V4L2: %zu buffers allocated (mmap)", buffers_.size());
    return true;
}

// ── read() 模式降级 ────────────────────────────────────────

bool V4L2Capture::try_read_mode() {
    // read() 模式不需要 mmap 缓冲区，直接读取即可
    use_read_mode_ = true;
    qDebug("V4L2: using read() mode for %s", device_name_.c_str());
    return true;
}

// ── 流控制 ─────────────────────────────────────────────────

bool V4L2Capture::start_stream() {
    if (use_read_mode_) {
        // read() 模式不需要启动 streaming
        return true;
    }

    // 将所有缓冲区加入队列
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (::ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            emit error_occurred("VIDIOC_QBUF failed");
            return false;
        }
    }

    // 启动流
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        emit error_occurred("VIDIOC_STREAMON failed");
        return false;
    }

    return true;
}

bool V4L2Capture::stop_stream() {
    if (use_read_mode_) {
        return true;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ::ioctl(fd_, VIDIOC_STREAMOFF, &type);
    return true;
}

bool V4L2Capture::start() {
    if (fd_ < 0 || running_) return false;

    if (!start_stream()) {
        return false;
    }

    running_ = true;
    frame_count_ = 0;

    // 立即捕获第一帧
    capture_frame();

    qDebug("V4L2: capture started (%dx%d @ %dfps%s)", width_, height_, fps_,
           use_read_mode_ ? ", read mode" : "");
    return true;
}

void V4L2Capture::stop() {
    if (!running_) return;

    running_ = false;
    capture_timer_->stop();
    stop_stream();

    qDebug("V4L2: capture stopped (%lld frames)", (long long)frame_count_);
}

// ── 帧捕获 ─────────────────────────────────────────────────

void V4L2Capture::capture_frame() {
    if (!running_ || fd_ < 0) return;

    // 使用 select 等待数据
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000; // 200ms timeout

    int ret = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret < 0) {
        if (errno == EINTR) {
            capture_timer_->start(10);
        } else {
            emit error_occurred("select() failed: " + std::string(strerror(errno)));
        }
        return;
    }

    if (ret == 0) {
        capture_timer_->start(10);
        return;
    }

    const uint8_t* data = nullptr;
    size_t data_len = 0;

    if (use_read_mode_) {
        // read() 模式 — 直接从设备读取帧
        std::vector<uint8_t> read_buf(2 * 1024 * 1024); // 2MB 最大帧
        ssize_t n = ::read(fd_, read_buf.data(), read_buf.size());
        if (n > 0) {
            data = read_buf.data();
            data_len = static_cast<size_t>(n);
        }
        // 调度下一帧（read 模式使用固定间隔）
        int interval_ms = 1000 / fps_;
        capture_timer_->start(interval_ms);
    } else {
        // mmap 模式 — 出队已填充的缓冲区
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (::ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                capture_timer_->start(10);
                return;
            }
            emit error_occurred("VIDIOC_DQBUF failed: " + std::string(strerror(errno)));
            return;
        }

        if (buf.bytesused > 0 && buf.index < buffers_.size()) {
            data = static_cast<const uint8_t*>(buffers_[buf.index].start);
            data_len = buf.bytesused;
        }

        // 重新入队缓冲区
        ::ioctl(fd_, VIDIOC_QBUF, &buf);

        // 调度下一帧
        int interval_ms = 1000 / fps_;
        capture_timer_->start(std::max(1, interval_ms - 10));
    }

    // 处理帧数据
    if (data && data_len > 0) {
        QImage frame_img;

        // 尝试 JPEG/MJPEG 解码
        if (data_len >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
            frame_img = QImage::fromData(data, static_cast<int>(data_len), "JPEG");
        }

        // YUYV 格式 — raw frame
        if (frame_img.isNull() && data_len >= static_cast<size_t>(width_ * height_)) {
            frame_img = QImage(width_, height_, QImage::Format_RGB32);
            frame_img.fill(qRgb(40, 50, 70));
            // 尝试将 YUYV 前 1/4 像素渲染
            if (data_len >= static_cast<size_t>(width_ * height_ * 2)) {
                // 有足够的 YUYV 数据，简化渲染灰度
                QImage yuv_preview(width_, height_, QImage::Format_Grayscale8);
                for (int y = 0; y < height_ && y < 100; ++y) {
                    for (int x = 0; x < width_ && x < 100; ++x) {
                        int idx = y * width_ * 2 + x * 2;
                        if (idx + 1 < static_cast<int>(data_len)) {
                            yuv_preview.setPixel(x, y, qRgb(data[idx], data[idx], data[idx]));
                        }
                    }
                }
            }
        }

        if (!frame_img.isNull()) {
            QVideoFrame video_frame(frame_img);
            video_frame.setStartTime(frame_count_ * 33'333);
            emit frame_captured(video_frame);
            frame_count_++;
        } else if (frame_count_ % 30 == 0) {
            qDebug("V4L2: skip frame #%lld (size=%zu)", (long long)frame_count_, data_len);
        }
    }
}
