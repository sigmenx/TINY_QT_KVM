#include "drv_camera.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <cstdlib>
#include <QDebug>


CameraDevice::CameraDevice(QObject *parent) : QObject(parent),
    m_fd(-1), m_isCapturing(false), m_buffers(nullptr), m_nBuffers(0)
{
    // 默认初始化
    m_bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

CameraDevice::~CameraDevice()
{
    closeDevice();
}

bool CameraDevice::openDevice(const QString &devicePath)
{
    closeDevice();
    m_devicePath = devicePath;
    m_fd = ::open(devicePath.toLocal8Bit().data(), O_RDWR | O_NONBLOCK); // 非阻塞模式
    if (m_fd < 0) {
        perror("Open device failed");
        return false;
    }

    // 【关键修改】打开设备后立即检测 API 类型
    probeBufferType();

    return true;
}

// 检测设备使用的是 Single-planar 还是 Multi-planar
void CameraDevice::probeBufferType()
{
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("Query Cap failed");
        return;
    }

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        m_bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        qDebug() << "[Camera] Detected MPLANE Device";
    } else {
        m_bufType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        qDebug() << "[Camera] Detected Standard Capture Device";
    }
}

void CameraDevice::closeDevice()
{
    stopCapturing();
    usleep(20000);
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool CameraDevice::isOpened() const { return m_fd != -1; }
bool CameraDevice::isCapturing() const { return m_isCapturing; }

// ================= V4L2 查询逻辑 =================

QList<QPair<QString, unsigned int>> CameraDevice::getSupportedFormats()
{
    QList<QPair<QString, unsigned int>> formats;
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));

    // 【关键修改】使用检测到的类型
    fmtdesc.type = m_bufType;

    for (int i = 0; ; ++i) {
        fmtdesc.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0) break;
        formats.append({QString((char*)fmtdesc.description), fmtdesc.pixelformat});
    }
    return formats;
}

QList<QSize> CameraDevice::getResolutions(unsigned int pixelFormat)
{
    QList<QSize> sizes;
    struct v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = pixelFormat;

    for (int i = 0; ; ++i) {
        frmsize.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) break;
        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            sizes.append(QSize(frmsize.discrete.width, frmsize.discrete.height));
        } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
             // RK3566 HDMI-IN 有时返回 Stepwise，需要手动添加常用分辨率
             sizes.append(QSize(1920, 1080));
             sizes.append(QSize(1280, 720));
             sizes.append(QSize(640, 480));
             break; // Stepwise 只需解析一次
        }
    }
    return sizes;
}

QList<int> CameraDevice::getFramerates(unsigned int pixelFormat, int width, int height)
{
    QList<int> fpsList;
    struct v4l2_frmivalenum frmival;
    memset(&frmival, 0, sizeof(frmival));
    frmival.pixel_format = pixelFormat;
    frmival.width = width;
    frmival.height = height;

    for (int i = 0; ; ++i) {
        frmival.index = i;
        if (ioctl(m_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) < 0) break;
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            fpsList.append(frmival.discrete.denominator / frmival.discrete.numerator);
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE || frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
            // 同样处理 Stepwise，默认给 60 和 30
            fpsList << 60 << 30;
            break;
        }
    }
    if (fpsList.isEmpty()) fpsList << 30; // 兜底
    return fpsList;
}

// ================= 采集控制 =================

bool CameraDevice::startCapturing(int width, int height, unsigned int pixelFormat, int fps)
{
    if (m_fd < 0) return false;
    stopCapturing();

    // 1. 设置格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = m_bufType; // 使用探测到的类型

    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        // --- MPLANE 设置 ---
        fmt.fmt.pix_mp.width = width;
        fmt.fmt.pix_mp.height = height;
        fmt.fmt.pix_mp.pixelformat = pixelFormat;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE; // 这里的 HDMI 芯片通常是逐行
        fmt.fmt.pix_mp.num_planes = 1; // UYVY/RGBP 都是单平面打包格式
    } else {
        // --- Standard 设置 ---
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = pixelFormat;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("Set Format Failed");
        return false;
    }

    // 回读实际设置的参数
    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        m_width = fmt.fmt.pix_mp.width;
        m_height = fmt.fmt.pix_mp.height;
    } else {
        m_width = fmt.fmt.pix.width;
        m_height = fmt.fmt.pix.height;
    }
    m_pixelFormat = pixelFormat;

    // 2. 设置帧率
    struct v4l2_streamparm streamparm;
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = m_bufType; // 【关键】类型要一致
    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = fps;
    } else {
        streamparm.parm.capture.timeperframe.numerator = 1;
        streamparm.parm.capture.timeperframe.denominator = fps;
    }
    ioctl(m_fd, VIDIOC_S_PARM, &streamparm);

    // 3. 申请缓存
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = m_bufType; // 【关键】
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("ReqBufs Failed");
        return false;
    }

    // 4. Mmap
    m_buffers = (VideoBuffer*)calloc(req.count, sizeof(*m_buffers));
    m_nBuffers = req.count;
    if (!initMmap()) return false; // 封装了 Mmap 逻辑

    // 5. 开启流
    enum v4l2_buf_type type = (v4l2_buf_type)m_bufType;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("StreamOn Failed");
        return false;
    }

    // 6. 预分配 RGB 缓冲区
    if (m_pixelFormat == V4L2_PIX_FMT_YUYV ||
        m_pixelFormat == V4L2_PIX_FMT_UYVY ||
        m_pixelFormat == V4L2_PIX_FMT_RGB565) {
        m_rgbBuffer.resize(m_width * m_height * 3);
    }

    m_isCapturing = true;
    return true;
}

void CameraDevice::stopCapturing()
{
    if (!m_isCapturing) return;

    enum v4l2_buf_type type = (v4l2_buf_type)m_bufType;
    ioctl(m_fd, VIDIOC_STREAMOFF, &type);

    freeMmap();
    if (m_buffers) {
        free(m_buffers);
        m_buffers = nullptr;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = m_bufType;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(m_fd, VIDIOC_REQBUFS, &req);

    m_nBuffers = 0;
    m_isCapturing = false;
}

bool CameraDevice::initMmap()
{
    for (unsigned int i = 0; i < m_nBuffers; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1]; // MPLANE 需要 plane 结构
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = m_bufType;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1; // 对于 UYVY/RGBP 这种 Packed 格式，Plane 数为 1
        }

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("QueryBuf Failed");
            return false;
        }

        if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            m_buffers[i].length = planes[0].length;
            m_buffers[i].start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, planes[0].m.mem_offset);
        } else {
            m_buffers[i].length = buf.length;
            m_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
        }

        if (m_buffers[i].start == MAP_FAILED) return false;

        // 入队
        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) return false;
    }
    return true;
}

void CameraDevice::freeMmap()
{
    for (unsigned int i = 0; i < m_nBuffers; ++i) {
        if (m_buffers[i].start != MAP_FAILED)
            munmap(m_buffers[i].start, m_buffers[i].length);
    }
}

// 1. 出队
uint8_t* CameraDevice::dequeue(size_t &out_len, int &out_index)
{
    if (!m_isCapturing || !m_buffers || m_fd < 0) return nullptr;
//====================================================================
    // 1. 使用 select 等待数据 (避免非阻塞模式下的 CPU 空转)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms 超时
    // 即使 fd 是非阻塞的，select 依然会阻塞在这里等待，直到有数据或超时
    int r = select(m_fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) {
        // r=0 (超时) 或 r<0 (错误)
        return nullptr;
    }
//====================================================================
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = m_bufType;
    buf.memory = V4L2_MEMORY_MMAP;

    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
    }

    // 在非阻塞模式下，如果 select 说有数据，但 ioctl 依然拿不到（极罕见），
    // ioctl 会返回 -1 且 errno=EAGAIN。这不会卡死，只是这次返回失败而已。
    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        // 如果是 EAGAIN，说明只是暂时没数据，忽略即可
        if (errno != EAGAIN) {
            perror("DQBUF failed");
        }
        return nullptr;
    }

    out_index = buf.index;

    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        out_len = planes[0].bytesused;
    } else {
        out_len = buf.bytesused;
    }

    return static_cast<uint8_t*>(m_buffers[buf.index].start);
}

// 2. 入队
void CameraDevice::enqueue(int index)
{
    if (m_fd < 0 || index < 0) return;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = m_bufType;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if (m_bufType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
    }

    ioctl(m_fd, VIDIOC_QBUF, &buf);
}

// 3. 转换：Raw -> QImage
void CameraDevice::toQImage(const uint8_t* rawData, size_t len, QImage &outImage)
{
    if (!rawData) return;
    // 分支 1: YUYV (4:2:2 Packed)
    if (m_pixelFormat == V4L2_PIX_FMT_YUYV) {
        // 软转码：YUYV -> RGB (写入 m_rgbBuffer)
        yuyv_to_rgb(rawData, m_rgbBuffer.data(), m_width, m_height);

        // 深拷贝：生成独立的 QImage，防止 m_rgbBuffer 在下一帧被覆盖时影响 UI 显示
        // 注意：QImage(data, ...) 构造函数默认浅拷贝，必须加 .copy()
        outImage = QImage(m_rgbBuffer.data(), m_width, m_height, QImage::Format_RGB888).copy();

    }
    // 分支 2: UYVY (4:2:2 Packed)
    else if (m_pixelFormat == V4L2_PIX_FMT_UYVY) {
        uyvy_to_rgb(rawData, m_rgbBuffer.data(), m_width, m_height);
        outImage = QImage(m_rgbBuffer.data(), m_width, m_height, QImage::Format_RGB888).copy();
    }
    // 分支 3: RGBP (RGB565 16bit)
    else if (m_pixelFormat == V4L2_PIX_FMT_RGB565) {
        rgb565_to_rgb(rawData, m_rgbBuffer.data(), m_width, m_height);
        outImage = QImage(m_rgbBuffer.data(), m_width, m_height, QImage::Format_RGB888).copy();
    }
    // 分支 4: MJPEG
    else if (m_pixelFormat == V4L2_PIX_FMT_MJPEG) {
        // MJPEG 直接解压
        outImage.loadFromData(rawData, len);
        // loadFromData 内部会自动创建深拷贝的内存
    }
}

//// [兼容接口] 旧逻辑的 wrapper
//bool CameraDevice::captureFrame(QImage &image)
//{
//    size_t len = 0;
//    int index = -1;

//    // 1. 获取原始数据
//    uint8_t* raw = dequeue(len, index);
//    if (!raw) return false;

//    // 2. 转换为图像
//    toQImage(raw, len, image);

//    // 3. 归还缓冲区
//    enqueue(index);

//    return true;
//}

// 内部算法 // YUYV: Y0 U0 Y1 V0
void CameraDevice::yuyv_to_rgb(const unsigned char *yuyv, unsigned char *rgb, int width, int height)
{
    int y0, u, y1, v;
    int r0, g0, b0, r1, g1, b1;
    int i = 0, j = 0;

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col += 2) {
            y0 = yuyv[i++]; u  = yuyv[i++];
            y1 = yuyv[i++]; v  = yuyv[i++];

            // 你的原始优化算法
            r0 = y0 + 1.402 * (v - 128);
            g0 = y0 - 0.34414 * (u - 128) - 0.71414 * (v - 128);
            b0 = y0 + 1.772 * (u - 128);

            r1 = y1 + 1.402 * (v - 128);
            g1 = y1 - 0.34414 * (u - 128) - 0.71414 * (v - 128);
            b1 = y1 + 1.772 * (u - 128);

            auto clamp = [](int x) { return (x < 0) ? 0 : ((x > 255) ? 255 : x); };
            rgb[j++] = clamp(r0); rgb[j++] = clamp(g0); rgb[j++] = clamp(b0);
            rgb[j++] = clamp(r1); rgb[j++] = clamp(g1); rgb[j++] = clamp(b1);
        }
    }
}
// UYVY: U0 Y0 V0 Y1 (与 YUYV 只是字节序不同)
void CameraDevice::uyvy_to_rgb(const unsigned char *uyvy, unsigned char *rgb, int width, int height)
{
    int y0, u, y1, v;
    int i = 0, j = 0;
    auto clamp = [](int x) { return (x < 0) ? 0 : ((x > 255) ? 255 : x); };

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col += 2) {
            // UYVY 排列
            u  = uyvy[i++]; y0 = uyvy[i++];
            v  = uyvy[i++]; y1 = uyvy[i++];

            // 下面的公式与 YUYV 完全一致
            int c = y0 - 16;
            int d = u - 128;
            int e = v - 128;
            rgb[j++] = clamp((298 * c + 409 * e + 128) >> 8);           // R
            rgb[j++] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            rgb[j++] = clamp((298 * c + 516 * d + 128) >> 8);           // B

            c = y1 - 16;
            rgb[j++] = clamp((298 * c + 409 * e + 128) >> 8);           // R
            rgb[j++] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            rgb[j++] = clamp((298 * c + 516 * d + 128) >> 8);           // B
        }
    }
}

// RGB565 -> RGB888
void CameraDevice::rgb565_to_rgb(const unsigned char *raw, unsigned char *rgb, int width, int height)
{
    const uint16_t *src = (const uint16_t *)raw;
    int j = 0;
    int totalPixels = width * height;

    for (int i = 0; i < totalPixels; ++i) {
        uint16_t pixel = src[i];

        // 提取 5-6-5 分量
        uint8_t r5 = (pixel >> 11) & 0x1F;
        uint8_t g6 = (pixel >> 5) & 0x3F;
        uint8_t b5 = pixel & 0x1F;

        // 扩展到 8 位 (R5->R8: x << 3 | x >> 2)
        rgb[j++] = (r5 << 3) | (r5 >> 2);
        rgb[j++] = (g6 << 2) | (g6 >> 4);
        rgb[j++] = (b5 << 3) | (b5 >> 2);
    }
}
