#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  RkmpiCapture — camera capture via RKMPI VI + VPSS on RV1106
//
//  Replaces V4L2Capture for the rkmpi build target.
//
//  Pipeline
//  ────────
//  RKMPI VI (IMX415, NV12, 1920×1080 @ 30 fps)
//    └─[SYS_Bind]─► VPSS Group 0
//                      ├─ Chn 0 (320×320 NV12)  ← inference
//                      └─ Chn 1 (640×480 NV12)  ← MJPEG stream (Step 2)
//
//  Threading
//  ─────────
//  open()  — brings up VI + VPSS, starts the fetch thread.
//  start() — starts the dispatch thread which calls the user callback.
//  stop()  — stops both threads and tears down RKMPI.
//
//  Fetch thread  : RK_MPI_VPSS_GetChnFrame → copy NV12 → release VPSS buffer
//                  (VPSS buffer released immediately so VPSS is never stalled)
//                  → push RawFrame to a 1-slot queue (drop oldest).
//
//  Dispatch thread: pop RawFrame → NV12→RGB(320×320) → letterbox float CHW
//                   → call FrameCallback.
//
//  This decouples the 15ms rknn_run from the hardware buffer lifetime so
//  VPSS can fill the next buffer while the NPU runs on the previous one.
//
//  ISP note
//  ────────
//  rkipc must be stopped before open() — it holds the VI dev exclusively.
//  Do NOT start rkaiq_3A_server. ISP runs with static config from the
//  kernel device tree (same "green tint without AWB" as the V4L2 path).
//  Manual white-balance gains in MjpegStreamerConfig compensate.
//
//  IRQ note
//  ────────
//  Keep bypass_irq_handler=1 in S99trap. VPSS uses RGA2 interrupts (different
//  IRQ from the NPU completion IRQ 37). The ISP DMA / NPU IRQ priority
//  conflict is eliminated by polling mode — unrelated to VPSS.
//
//  Usage
//  ─────
//    RkmpiCapture cam;
//    cam.setCallback([&](const CaptureFrame& f) { /* inference */ });
//    cam.setErrorCallback([&](const std::string& msg) { ... });
//    cam.open(cfg);
//    cam.start();
//    ...
//    cam.stop();
// ─────────────────────────────────────────────────────────────────────────────

#include "float_mat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Config ────────────────────────────────────────────────────────────────────

struct RkmpiConfig {
    // VI device / pipe / channel IDs — normally 0 on a single-camera board.
    int viDev  = 0;
    int viPipe = 0;
    int viChn  = 0;

    // Sensor capture resolution (must match the ISP pipe configured in DTB).
    int captureWidth  = 1920;
    int captureHeight = 1080;
    int framerate     = 30;

    // Number of VI buffers allocated for the VPSS bind path.
    // Must be ≥ 2; u32Depth is forced to 0 (bind mode).
    int bufferCount = 4;

    // VPSS output resolution for inference (channel 0).
    int modelWidth  = 320;
    int modelHeight = 320;

    // VPSS output resolution for MJPEG streaming (channel 1).
    int streamWidth  = 640;
    int streamHeight = 480;
};

// ── Frame delivered to the callback ───────────────────────────────────────────

struct CaptureFrame {
    // Compact NV12 at streamWidth×streamHeight (from VPSS channel 1).
    // Used by MjpegStreamer.  Empty until Step 2 wires channel 1.
    std::vector<uint8_t> nv12;
    int width  = 0;
    int height = 0;

    // Preprocessed model input: CHW float32 [0,1], modelWidth×modelHeight.
    // Allocated once per frame in the dispatch thread.
    FloatMat modelInput;

    // Letterbox metadata — passed to YoloDecoder to map detections back to
    // original sensor coordinates.
    float scale   = 1.f;
    float padLeft = 0.f;
    float padTop  = 0.f;

    uint64_t frameId    = 0;
    uint64_t timestampNs = 0;
};

// ── RkmpiCapture ──────────────────────────────────────────────────────────────

using FrameCallback = std::function<void(const CaptureFrame&)>;
using ErrorCallback = std::function<void(const std::string&)>;

class RkmpiCapture {
public:
    RkmpiCapture()  = default;
    ~RkmpiCapture();

    RkmpiCapture(const RkmpiCapture&)            = delete;
    RkmpiCapture& operator=(const RkmpiCapture&) = delete;

    void setCallback(FrameCallback cb)      { m_frameCb = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { m_errorCb = std::move(cb); }

    // Initialise RKMPI (MPI_SYS_Init, VI, VPSS, bind).
    // Throws std::runtime_error on failure.
    void open(const RkmpiConfig& cfg);

    // Start fetch + dispatch threads.
    void start();

    // Stop threads and tear down RKMPI pipeline.
    void stop();

    bool isRunning() const { return m_running.load(); }

    void printStats() const;

private:
    // ── Fetch thread: VPSS GetChnFrame → copy → release → push to queue ──────
    void fetchLoop();

    // ── Dispatch thread: pop queue → preprocess → callback ───────────────────
    void dispatchLoop();

    // ── RKMPI setup / teardown ─────────────────────────────────────────────────
    void initVI();
    void initVPSS();
    void bindVItoVPSS();
    void teardown();

    // ── One-slot frame queue (drop-oldest) ────────────────────────────────────
    struct RawFrame {
        std::vector<uint8_t> nv12_model;  // 320×320 compact NV12
        std::vector<uint8_t> nv12_stream; // 640×480 compact NV12 (Step 2)
        uint64_t frameId     = 0;
        uint64_t timestampNs = 0;
    };

    RawFrame                 m_queue;
    bool                     m_queueHasFrame = false;
    std::mutex               m_queueMutex;
    std::condition_variable  m_queueCv;

    // ── State ──────────────────────────────────────────────────────────────────
    RkmpiConfig  m_cfg;
    FrameCallback m_frameCb;
    ErrorCallback m_errorCb;

    std::thread  m_fetchThread;
    std::thread  m_dispatchThread;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shouldStop{false};

    uint64_t m_frameCount    = 0;
    uint64_t m_droppedFrames = 0;

    // ── Stats ──────────────────────────────────────────────────────────────────
    mutable std::mutex m_statMutex;
};
