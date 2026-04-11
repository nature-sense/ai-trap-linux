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
//                      ├─ Chn 0 (320×320  NV12)  ← inference (GetChnFrame)
//                      ├─ Chn 1 (640×480  NV12)  ← [SYS_Bind]─► VENC (MJPEG)
//                      └─ Chn 2 (1920×1080 NV12) ← full-res crops (GetChnFrame)
//
//  Threading
//  ─────────
//  open()  — brings up VI + VPSS, starts the fetch thread.
//  start() — starts the dispatch thread which calls the user callback.
//  stop()  — stops both threads and tears down RKMPI.
//
//  Fetch thread  : GetChnFrame(chn0 inf) + GetChnFrame(chn2 full) → copy NV12
//                  → release VPSS buffers immediately → 1-slot queue (drop oldest).
//                  Chn1 (stream) is bound directly to VENC — not fetched here.
//
//  Dispatch thread: pop RawFrame → NV12→RGB(320×320) → letterbox float CHW
//                   → call FrameCallback.
//
//  VENC thread   : RK_MPI_VENC_GetStream → fire JpegCallback → ReleaseStream.
//
//  This decouples the 15ms rknn_run from the hardware buffer lifetime so
//  VPSS can fill the next buffer while the NPU runs on the previous one.
//
//  ISP note
//  ────────
//  rkipc must be stopped before open() — it holds the VI dev exclusively.
//  rkaiq is used for one-shot 3A convergence (see main_rkmpi.cpp): it runs
//  for ~5 s before open() so AE/AWB/CCM lock, then stops with
//  keep_ext_hw_st=true so the ISP hardware retains calibrated register state.
//  After rkaiq stops, the ISP continues with fixed AE/AWB/CCM — no ongoing
//  rkaiq DDR DMA that would starve the NPU.  wbR/wbG/wbB are set to 1.0
//  when rkaiq has converged (ISP hardware handles WB in hardware).
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

    // VPSS output resolution for MJPEG streaming (channel 1, bound to VENC).
    int streamWidth  = 640;
    int streamHeight = 480;

    // VENC MJPEG quality factor (1–99).  Higher = better quality, larger JPEG.
    int jpegQuality = 75;

    // Set false to skip VENC init and VPSS chn1 — useful for testing inference
    // without hardware MJPEG encoding.
    bool enableVenc = true;

    // Software white-balance gains applied after NV12→RGB in softwareJpegLoop.
    // Compensates for IMX415 Bayer sensor running without rkaiq AWB/CCM on RV1106:
    // the raw ISP output has a strong green bias (Bayer GBRG, 2:1 green pixels)
    // and is underexposed (no auto-exposure).
    // Empirically measured from a white target: R≈39, G≈101, B≈61 raw → target 180.
    // Applied only to the stream JPEG — not to the model NV12 input.
    float wbR = 4.57f;
    float wbG = 1.77f;
    float wbB = 2.95f;
};

// ── Frame delivered to the callback ───────────────────────────────────────────

struct CaptureFrame {
    // Full-resolution compact NV12 (captureWidth×captureHeight, from VPSS chn2).
    // Used by CropSaver — MJPEG streaming is handled by VENC via JpegCallback.
    std::vector<uint8_t> nv12_fullres;
    int width  = 0;   // capture width  (e.g. 1920)
    int height = 0;   // capture height (e.g. 1080)

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
// Called from the VENC thread with each hardware-encoded JPEG frame.
// `data` is valid only for the duration of the callback.
using JpegCallback  = std::function<void(const uint8_t* data, size_t len)>;

class RkmpiCapture {
public:
    RkmpiCapture()  = default;
    ~RkmpiCapture();

    RkmpiCapture(const RkmpiCapture&)            = delete;
    RkmpiCapture& operator=(const RkmpiCapture&) = delete;

    void setCallback(FrameCallback cb)      { m_frameCb = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { m_errorCb = std::move(cb); }
    void setJpegCallback(JpegCallback cb)   { m_jpegCb  = std::move(cb); }

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

    // ── VENC thread: GetStream → JpegCallback → ReleaseStream ────────────────
    void vencLoop();

    // ── Software JPEG thread: VPSS CHN_STR → NV12→RGB → stb JPEG → callback ──
    void softwareJpegLoop();

    // ── RKMPI setup / teardown ─────────────────────────────────────────────────
    void initVI();
    void startVI();
    void initVPSS();
    void initVENC();
    void bindVItoVPSS();
    void teardown();

    // ── One-slot frame queue (drop-oldest) ────────────────────────────────────
    struct RawFrame {
        std::vector<uint8_t> nv12_model;    // 320×320 compact NV12
        std::vector<uint8_t> nv12_fullres;  // captureW×captureH compact NV12
        uint64_t frameId     = 0;
        uint64_t timestampNs = 0;
    };

    RawFrame                 m_queue;
    bool                     m_queueHasFrame = false;
    std::mutex               m_queueMutex;
    std::condition_variable  m_queueCv;

    // ── State ──────────────────────────────────────────────────────────────────
    RkmpiConfig   m_cfg;
    FrameCallback m_frameCb;
    ErrorCallback m_errorCb;
    JpegCallback  m_jpegCb;

    std::thread  m_fetchThread;
    std::thread  m_dispatchThread;
    std::thread  m_vencThread;
    std::thread  m_softJpegThread;
    bool         m_vencInitialised = false;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shouldStop{false};

    uint64_t m_frameCount    = 0;
    uint64_t m_droppedFrames = 0;

    // ── Stats ──────────────────────────────────────────────────────────────────
    mutable std::mutex m_statMutex;
};
