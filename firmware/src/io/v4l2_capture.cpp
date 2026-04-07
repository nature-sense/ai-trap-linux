#include "v4l2_capture.h"
#include "imgproc.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <chrono>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/videodev2.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool V4L2Capture::xioctl(int request, void* arg) const {
    int r;
    do {
        r = ioctl(m_fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r != -1;
}

void V4L2Capture::emitError(const std::string& msg) {
    if (m_errorCb) m_errorCb(msg);
    else fprintf(stderr, "[V4L2Capture] %s\n", msg.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

V4L2Capture::V4L2Capture()  = default;

V4L2Capture::~V4L2Capture() {
    if (m_running.load())
        stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Callbacks
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::setCallback(FrameCallback cb)      { m_frameCb = std::move(cb); }
void V4L2Capture::setErrorCallback(ErrorCallback cb) { m_errorCb = std::move(cb); }

// ─────────────────────────────────────────────────────────────────────────────
//  open()
//
//  1. Open the device node.
//  2. Verify it is a capture device.
//  3. Negotiate NV12 format at the requested resolution.
//     The driver may adjust the resolution — we accept whatever it offers.
//  4. Set framerate via VIDIOC_S_PARM.
//  5. Allocate MMAP buffers and mmap each one.
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::open(const V4L2Config& cfg) {
    m_cfg = cfg;

    // ── 1. Open device ────────────────────────────────────────────────────────
    m_fd = ::open(cfg.device.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0)
        throw std::runtime_error("V4L2Capture: cannot open " + cfg.device +
                                 ": " + strerror(errno));

    // ── 2. Verify capture capability ──────────────────────────────────────────
    v4l2_capability cap{};
    if (!xioctl(VIDIOC_QUERYCAP, &cap))
        throw std::runtime_error("V4L2Capture: VIDIOC_QUERYCAP failed: " +
                                 std::string(strerror(errno)));

    const bool hasSingle = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool hasMplane = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;

    if (!hasSingle && !hasMplane)
        throw std::runtime_error("V4L2Capture: " + cfg.device +
                                 " is not a video capture device");

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        throw std::runtime_error("V4L2Capture: " + cfg.device +
                                 " does not support streaming");

    // Prefer single-plane; fall back to MPLANE (RV1106 rkisp is MPLANE-only)
    m_isMplane = !hasSingle && hasMplane;
    const v4l2_buf_type bufType = m_isMplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    printf("V4L2Capture: %s  mode=%s\n",
           cfg.device.c_str(), m_isMplane ? "MPLANE" : "single-plane");

    // ── 3. Negotiate NV12 format ──────────────────────────────────────────────
    v4l2_format fmt{};
    fmt.type = bufType;

    if (m_isMplane) {
        fmt.fmt.pix_mp.width       = static_cast<uint32_t>(cfg.captureWidth);
        fmt.fmt.pix_mp.height      = static_cast<uint32_t>(cfg.captureHeight);
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    } else {
        fmt.fmt.pix.width          = static_cast<uint32_t>(cfg.captureWidth);
        fmt.fmt.pix.height         = static_cast<uint32_t>(cfg.captureHeight);
        fmt.fmt.pix.pixelformat    = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field          = V4L2_FIELD_NONE;
    }

    if (!xioctl(VIDIOC_S_FMT, &fmt))
        throw std::runtime_error("V4L2Capture: VIDIOC_S_FMT failed: " +
                                 std::string(strerror(errno)));

    // Driver may have adjusted resolution — accept it
    if (m_isMplane) {
        m_width      = static_cast<int>(fmt.fmt.pix_mp.width);
        m_height     = static_cast<int>(fmt.fmt.pix_mp.height);
        m_stride     = static_cast<int>(fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
        m_numPlanes  = fmt.fmt.pix_mp.num_planes;
        if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12)
            throw std::runtime_error("V4L2Capture: driver did not accept NV12 format");
    } else {
        m_width  = static_cast<int>(fmt.fmt.pix.width);
        m_height = static_cast<int>(fmt.fmt.pix.height);
        m_stride = static_cast<int>(fmt.fmt.pix.bytesperline);
        m_numPlanes = 1;
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12)
            throw std::runtime_error("V4L2Capture: driver did not accept NV12 format");
    }

    printf("V4L2Capture: %dx%d (requested %dx%d)  stride=%d  planes=%u  fmt=NV12\n",
           m_width, m_height,
           cfg.captureWidth, cfg.captureHeight,
           m_stride, m_numPlanes);

    // ── 4. Set framerate ──────────────────────────────────────────────────────
    v4l2_streamparm parm{};
    parm.type = bufType;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(cfg.framerate);

    if (!xioctl(VIDIOC_S_PARM, &parm))
        fprintf(stderr, "V4L2Capture: VIDIOC_S_PARM warning: %s "
                "(framerate may not be adjustable via this ioctl)\n",
                strerror(errno));
    else {
        int actualFps = static_cast<int>(
            parm.parm.capture.timeperframe.denominator /
            std::max(1u, parm.parm.capture.timeperframe.numerator));
        printf("V4L2Capture: framerate set to %d fps\n", actualFps);
    }

    // ── 5. Request MMAP buffers ───────────────────────────────────────────────
    v4l2_requestbuffers req{};
    req.count  = static_cast<uint32_t>(cfg.bufferCount);
    req.type   = bufType;
    req.memory = V4L2_MEMORY_MMAP;

    if (!xioctl(VIDIOC_REQBUFS, &req))
        throw std::runtime_error("V4L2Capture: VIDIOC_REQBUFS failed: " +
                                 std::string(strerror(errno)));

    if (req.count < 2)
        throw std::runtime_error("V4L2Capture: insufficient MMAP buffers allocated");

    printf("V4L2Capture: allocated %u MMAP buffers\n", req.count);

    // ── 6. mmap each buffer ───────────────────────────────────────────────────
    m_buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type   = bufType;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (m_isMplane) {
            buf.m.planes = planes;
            buf.length   = m_numPlanes;
        }

        if (!xioctl(VIDIOC_QUERYBUF, &buf))
            throw std::runtime_error("V4L2Capture: VIDIOC_QUERYBUF failed: " +
                                     std::string(strerror(errno)));

        for (uint32_t p = 0; p < m_numPlanes; p++) {
            size_t   len    = m_isMplane ? planes[p].length        : buf.length;
            uint32_t offset = m_isMplane ? planes[p].m.mem_offset  : buf.m.offset;

            m_buffers[i].length[p] = len;
            m_buffers[i].start[p]  = mmap(nullptr, len,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          m_fd, offset);

            if (m_buffers[i].start[p] == MAP_FAILED)
                throw std::runtime_error("V4L2Capture: mmap failed for buffer " +
                                         std::to_string(i) + " plane " +
                                         std::to_string(p) + ": " + strerror(errno));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  start()
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::start() {
    if (!m_frameCb)
        throw std::runtime_error("V4L2Capture: setCallback() must be called before start()");

    const v4l2_buf_type bufType = m_isMplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Queue all buffers
    for (uint32_t i = 0; i < m_buffers.size(); i++) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type   = bufType;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (m_isMplane) {
            buf.m.planes = planes;
            buf.length   = m_numPlanes;
        }
        if (!xioctl(VIDIOC_QBUF, &buf))
            throw std::runtime_error("V4L2Capture: VIDIOC_QBUF failed: " +
                                     std::string(strerror(errno)));
    }

    // Start streaming
    v4l2_buf_type type = bufType;
    if (!xioctl(VIDIOC_STREAMON, &type))
        throw std::runtime_error("V4L2Capture: VIDIOC_STREAMON failed: " +
                                 std::string(strerror(errno)));

    m_shouldStop = false;
    m_running    = true;

    m_captureThread  = std::thread([this] { captureLoop();  });
    m_dispatchThread = std::thread([this] { dispatchLoop(); });

    printf("V4L2Capture: streaming started\n");
}

// ─────────────────────────────────────────────────────────────────────────────
//  stop()
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::stop() {
    m_shouldStop = true;
    m_queueCv.notify_all();

    if (m_captureThread.joinable())  m_captureThread.join();
    if (m_dispatchThread.joinable()) m_dispatchThread.join();

    // Stop streaming
    v4l2_buf_type type = m_isMplane
        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(VIDIOC_STREAMOFF, &type);

    // Unmap buffers
    for (auto& buf : m_buffers)
        for (uint32_t p = 0; p < m_numPlanes; p++)
            if (buf.start[p] && buf.start[p] != MAP_FAILED)
                munmap(buf.start[p], buf.length[p]);
    m_buffers.clear();

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    m_running = false;
    printf("V4L2Capture: stopped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
//  captureLoop  — runs on m_captureThread
//
//  Uses select() to wait for a frame with a 1-second timeout so we can
//  check m_shouldStop without blocking forever on VIDIOC_DQBUF.
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::captureLoop() {
    while (!m_shouldStop.load()) {

        // Wait for a frame to be ready (1 second timeout)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_fd, &fds);
        timeval tv{};
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int r = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == -1) {
            if (errno == EINTR) continue;
            emitError("captureLoop: select() error: " + std::string(strerror(errno)));
            break;
        }
        if (r == 0) continue;  // timeout — loop and check m_shouldStop

        // Dequeue a filled buffer
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type   = m_isMplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (m_isMplane) {
            buf.m.planes = planes;
            buf.length   = m_numPlanes;
        }

        if (!xioctl(VIDIOC_DQBUF, &buf)) {
            if (errno == EAGAIN) continue;
            emitError("captureLoop: VIDIOC_DQBUF error: " + std::string(strerror(errno)));
            break;
        }

        m_framesReceived++;
        m_frameId++;

        // Timestamp from kernel: v4l2_buffer.timestamp is a timeval (µs)
        int64_t timestampNs =
            static_cast<int64_t>(buf.timestamp.tv_sec)  * 1'000'000'000LL +
            static_cast<int64_t>(buf.timestamp.tv_usec) * 1'000LL;

        // ── De-stride NV12 into compact buffer ────────────────────────────────
        // Single-plane or MPLANE with num_planes=1: Y at offset 0, UV at S*H.
        // MPLANE with num_planes=2: plane[0]=Y, plane[1]=UV (separate mmaps).
        const int W = m_width;
        const int H = m_height;
        const int S = m_stride;   // bytes per Y row (may be > W with padding)

        const auto* yPlane  = static_cast<const uint8_t*>(m_buffers[buf.index].start[0]);
        const auto* uvPlane = (m_numPlanes >= 2)
            ? static_cast<const uint8_t*>(m_buffers[buf.index].start[1])
            : yPlane + S * H;

        std::vector<uint8_t> nv12(static_cast<size_t>(W * H * 3 / 2));

        // De-stride Y rows
        for (int row = 0; row < H; row++)
            std::memcpy(nv12.data() + row * W,
                        yPlane       + row * S,
                        static_cast<size_t>(W));

        // De-stride UV rows
        uint8_t* uvDst = nv12.data() + W * H;
        for (int row = 0; row < H / 2; row++)
            std::memcpy(uvDst   + row * W,
                        uvPlane + row * S,
                        static_cast<size_t>(W));

        // Re-queue the buffer immediately so the ISP stays fed
        if (m_isMplane) {
            buf.m.planes = planes;   // planes[] is still valid in this scope
            buf.length   = m_numPlanes;
        }
        if (!xioctl(VIDIOC_QBUF, &buf))
            emitError("captureLoop: VIDIOC_QBUF error: " + std::string(strerror(errno)));

        // Push to dispatch queue — drop oldest frame if queue is full
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            if (static_cast<int>(m_frameQueue.size()) >= MAX_QUEUE_DEPTH) {
                m_frameQueue.pop();
                m_framesDropped++;
            }
            m_frameQueue.push({
                m_frameId,
                timestampNs,
                std::move(nv12),
                W, H
            });
        }
        m_queueCv.notify_one();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  dispatchLoop  — runs on m_dispatchThread
//
//  Identical structure to LibcameraCapture::dispatchLoop.
//  Pops RawFrames, preprocesses, fires callback.
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::dispatchLoop() {
    while (true) {
        RawFrame raw;
        {
            std::unique_lock<std::mutex> lk(m_queueMutex);
            m_queueCv.wait(lk, [this] {
                return !m_frameQueue.empty() || m_shouldStop.load();
            });
            if (m_frameQueue.empty()) break;
            raw = std::move(m_frameQueue.front());
            m_frameQueue.pop();
        }

        CaptureFrame frame;
        frame.frameId     = raw.frameId;
        frame.timestampNs = raw.timestampNs;
        frame.width       = raw.width;
        frame.height      = raw.height;

        frame.modelInput = preprocess(raw.nv12.data(),
                                      raw.width, raw.height,
                                      frame.scale, frame.padLeft, frame.padTop);

        frame.nv12 = std::move(raw.nv12);

        if (m_frameCb)
            m_frameCb(frame);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  preprocess  — NV12 → letterboxed normalised RGB FloatMat (CHW, [0,1])
// ─────────────────────────────────────────────────────────────────────────────

FloatMat V4L2Capture::preprocess(
    const uint8_t* nv12,
    int width, int height,
    float& scale, int& padLeft, int& padTop) const
{
    const int dstW = m_cfg.modelWidth;
    const int dstH = m_cfg.modelHeight;

    // NV12 → packed RGB uint8 (HWC), BT.601 limited-range
    std::vector<uint8_t> rgbBuf(static_cast<size_t>(width * height * 3));
    nv12_to_rgb_u8(nv12, width, height, rgbBuf.data());

    // Letterbox: scale to fit dstW×dstH preserving aspect ratio
    scale   = std::min(static_cast<float>(dstW) / width,
                       static_cast<float>(dstH) / height);
    int newW = static_cast<int>(width  * scale);
    int newH = static_cast<int>(height * scale);
    padLeft  = (dstW - newW) / 2;
    padTop   = (dstH - newH) / 2;

    // Bilinear resize packed RGB uint8 to (newW × newH)
    std::vector<uint8_t> resized(static_cast<size_t>(newW * newH * 3));
    bilinear_resize_rgb_u8(rgbBuf.data(), width, height,
                           resized.data(), newW, newH);

    // Allocate 3-channel CHW float mat, fill with letterbox grey (114/255)
    FloatMat out(dstW, dstH, 3);
    out.fill(114.f / 255.f);

    // Copy resized pixels into each channel plane, normalising uint8 → [0,1]
    for (int c = 0; c < 3; ++c) {
        float*         dst = out.channel(c);
        const uint8_t* src = resized.data();
        for (int y = 0; y < newH; ++y) {
            float*         dstRow = dst + (padTop + y) * dstW + padLeft;
            const uint8_t* srcRow = src + y * newW * 3;
            for (int x = 0; x < newW; ++x)
                dstRow[x] = srcRow[x * 3 + c] * (1.f / 255.f);
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  printStats
// ─────────────────────────────────────────────────────────────────────────────

void V4L2Capture::printStats() const {
    printf("V4L2Capture: received=%-6llu  dropped=%-6llu\n",
           (unsigned long long)m_framesReceived.load(),
           (unsigned long long)m_framesDropped.load());
}
