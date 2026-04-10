#include "rkmpi_capture.h"
#include "decoder.h"
#include "tracker.h"
#include "persistence.h"
#include "crop_saver.h"
#include "mjpeg_streamer.h"
#include "sse_server.h"
#include "http_server.h"
#include "sync_manager.h"
#include "wifi_manager.h"
#include "rknn_infer.h"

// rkaiq ISP engine — AE/AWB/AF/CCM in ISP hardware.
// Enabled when librkaiq.so is present in the cross-compile sysroot
// (fetched by build-luckfox-mac.sh from the Luckfox SDK).
// If unavailable, the build falls back to empirical software WB correction.
//
// We use a minimal forward-declaration header (rkaiq_minimal.h) rather than
// the full rkaiq SDK headers, which have complex cross-directory dependencies
// across uAPI2/, common/, algos/, xcore/ and dozens of sub-headers.
#ifdef HAVE_RKAIQ
#include "rkaiq_minimal.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Class names
// ─────────────────────────────────────────────────────────────────────────────

static const char* CLASS_NAMES[1] = { "insect" };
static const char* className(int id) { return (id == 0) ? CLASS_NAMES[0] : "?"; }

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

// ─────────────────────────────────────────────────────────────────────────────
//  AsyncInferenceQueue
//
//  Decouples the RkmpiCapture dispatch thread from rknn_run.
//
//  The capture dispatch thread (which does NV12→RGB + float conversion) posts
//  CaptureFrames here.  A dedicated inference thread picks them up and runs
//  rknn_run(), decode, track, save, stream — without blocking the capture path.
//
//  This gives true CPU/NPU parallelism:
//    Capture thread:   preprocess frame N+1  ┐
//    Inference thread: rknn_run(frame N)      ┘ overlap
//
//  One-slot queue (drop-oldest): if inference takes longer than one frame
//  period the capture thread discards the older frame.  This keeps latency
//  bounded at the cost of occasional frame drops under heavy load.
// ─────────────────────────────────────────────────────────────────────────────

class AsyncInferenceQueue {
public:
    void push(CaptureFrame frame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hasFrame) ++m_dropped;
        m_frame    = std::move(frame);
        m_hasFrame = true;
        m_cv.notify_one();
    }

    // Blocks until a frame is available or stop() is called.
    // Returns false when the queue has been stopped.
    bool pop(CaptureFrame& out)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_hasFrame || m_stopped; });
        if (m_stopped && !m_hasFrame) return false;
        out        = std::move(m_frame);
        m_hasFrame = false;
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true;
        m_cv.notify_all();
    }

    uint64_t dropped() const { return m_dropped.load(); }

private:
    CaptureFrame             m_frame;
    bool                     m_hasFrame = false;
    bool                     m_stopped  = false;
    std::mutex               m_mutex;
    std::condition_variable  m_cv;
    std::atomic<uint64_t>    m_dropped{0};
};

// ─────────────────────────────────────────────────────────────────────────────
//  main
//
//  Usage: ./yolo_rkmpi [model.rknn] [db] [crops-dir]
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Line-buffer stdout so printf output appears immediately in log files.
    // By default stdout is fully-buffered when not connected to a TTY, which
    // causes stats and detection output to disappear into the buffer.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // Parse flags first, then positional args.
    // --no-rkaiq : skip rkaiq ISP engine init (use software WB; NPU runs unconstrained)
    bool flagNoRkaiq = false;
    int  firstPositional = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-rkaiq") {
            flagNoRkaiq = true;
        } else {
            // first non-flag positional argument starts here
            firstPositional = i;
            break;
        }
    }
    // If all args were flags, firstPositional sits past end — clamp defaults.
    const char* rknnFile = (firstPositional < argc) ? argv[firstPositional]     : "model.rknn";
    const char* dbPath   = (firstPositional+1 < argc) ? argv[firstPositional+1] : "detections.db";
    const char* cropsDir = (firstPositional+2 < argc) ? argv[firstPositional+2] : "crops";

    printf("══════════════════════════════════════════════════════════\n");
    printf("  YOLO11n  Luckfox Pico Zero  IMX415 RKMPI+VPSS + RKNN NPU\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  model : %s\n  db    : %s\n  crops : %s\n\n",
           rknnFile, dbPath, cropsDir);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    // rkaiq context — initialised after RKNN is ready, before cam.open().
    // Declared here so it is visible to both the init block and the shutdown block.
#ifdef HAVE_RKAIQ
    rk_aiq_sys_ctx_t* aiqCtx = nullptr;
#endif

    // ── Shared inference queue ────────────────────────────────────────────────
    AsyncInferenceQueue inferQueue;

    // ── Components (same as main_v4l2.cpp) ───────────────────────────────────

    SqliteWriter db;
    DecoderConfig decCfg;
    decCfg.numClasses  = 1;
    decCfg.confThresh  = 0.45f;
    decCfg.nmsThresh   = 0.45f;
    decCfg.modelWidth  = 320;
    decCfg.modelHeight = 320;
    YoloDecoder decoder(decCfg);
    ByteTracker tracker;

    try { db.open(dbPath); }
    catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open database: %s\n", e.what());
        return 1;
    }

    CropSaver crops;
    CropSaverConfig cropCfg;
    cropCfg.outputDir     = cropsDir;
    cropCfg.jpegQuality   = 90;
    cropCfg.minConfidence = 0.50f;
    cropCfg.maxQueueDepth = 16;
    try { crops.open(cropCfg); }
    catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open crop saver: %s\n", e.what());
        db.close(); return 1;
    }

    // ── MJPEG streamer ────────────────────────────────────────────────────────
    //
    // RKMPI VENC hardware-encodes each 640×480 VPSS frame into JPEG and fires
    // JpegCallback (set below on RkmpiCapture).  pushJpeg() bypasses the
    // software NV12→RGB→scale→encode path — zero CPU cost per frame.
    MjpegStreamer streamer;
    MjpegStreamerConfig streamCfg;
    streamCfg.port         = 9000;
    streamCfg.streamWidth  = 640;
    streamCfg.streamHeight = 480;
    streamCfg.jpegQuality  = 75;
    try { streamer.open(streamCfg); }
    catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot start streamer: %s\n", e.what());
        crops.close(); db.close(); return 1;
    }

    SseServer sse;
    SseConfig sseCfg;
    sseCfg.port = 8081;
    try { sse.open(sseCfg); }
    catch (const std::exception& e) {
        fprintf(stderr, "Warning: cannot start SSE server: %s\n", e.what());
    }

    SyncManager sync;
    sync.init(db.rawDb(), cropCfg.outputDir);

    float currentFps = 0.f;
    uint64_t g_frameCount = 0;
    std::atomic<bool> g_capturing{true};

    HttpServer http;
    HttpServerConfig httpCfg;
    httpCfg.port     = 8080;
    httpCfg.cropsDir = cropCfg.outputDir;
    httpCfg.trapId   = "luckfox_001";

    WifiConfig wifiCfg;
    wifiCfg.credsPath = "/opt/trap/wifi_creds.conf";
    WifiManager wifi(httpCfg.trapId, wifiCfg);
    wifi.applyStartupMode();

    try { http.open(httpCfg, &db, &sse, &sync, &currentFps, &g_capturing); }
    catch (const std::exception& e) {
        fprintf(stderr, "Warning: cannot start HTTP server: %s\n", e.what());
    }
    http.setWifiManager(&wifi);

    // ── RKNN model (load bytes only — context created in inference thread) ───────
    RknnInference net;
    if (!net.init(rknnFile, decCfg.modelWidth, decCfg.modelHeight)) {
        db.close(); return 1;
    }

    // ── Inference thread ──────────────────────────────────────────────────────
    //
    // IMPORTANT: the inference thread is started and the RKNN context is
    // fully initialised HERE — BEFORE RkmpiCapture::open() — to prevent a
    // hard reset caused by simultaneous DMA allocation:
    //
    //   rknn_init() + rknn_create_mem()  allocate DMA-coherent buffers via
    //   the NPU kernel driver.  RK_MPI_VI_EnableChn() / StreamOn similarly
    //   allocate large NV12 DMA buffers for the ISP pipeline.  When both
    //   run concurrently on the RV1106 AXI bus the system hard-resets.
    //
    //   Sequencing: RKNN DMA alloc → complete → RKMPI DMA alloc → streaming.
    //
    // Threading contract (librknnmrt same-thread requirement):
    //   rknn_init() and all subsequent rknn_run() calls must happen in the
    //   same thread.  eagerInit() is called here so the context lives
    //   entirely in this thread.

    // camCfg declared here so the inference thread lambda can capture it by ref
    // (the thread uses it to scale detection boxes to stream resolution).
    RkmpiConfig camCfg;
    camCfg.captureWidth  = 1920;
    camCfg.captureHeight = 1080;
    camCfg.framerate     = 0;   // 0 = VPSS passthrough; set > 0 to throttle group
    camCfg.bufferCount   = 4;
    camCfg.modelWidth    = 320;
    camCfg.modelHeight   = 320;
    camCfg.streamWidth   = 640;
    camCfg.streamHeight  = 480;
    camCfg.jpegQuality   = 75;
    camCfg.enableVenc    = false;  // hardware VENC (mpp_vcodec) broken on this BSP — use software JPEG

    std::mutex              rknnReadyMtx;
    std::condition_variable rknnReadyCV;
    bool                    rknnReady = false;
    bool                    rknnInitOk = false;

    std::thread inferThread([&]() {
        // Initialise NPU context before any RKMPI DMA begins.
        rknnInitOk = net.eagerInit();
        {
            std::lock_guard<std::mutex> lk(rknnReadyMtx);
            rknnReady = true;
        }
        rknnReadyCV.notify_one();
        if (!rknnInitOk) return;   // main thread will detect and exit

        CaptureFrame frame;
        while (inferQueue.pop(frame)) {
            // NPU inference
            FloatMat output;
            if (!net.infer(frame.modelInput.data(), output)) {
                fprintf(stderr, "[warn] rknn infer failed\n");
                continue;
            }

            // Decode YOLO output
            std::vector<Detection> dets = decoder.decode(
                output,
                frame.width, frame.height,
                frame.scale, frame.padLeft, frame.padTop);

            ++g_frameCount;

            // Track
            std::vector<TrackedObject> tracked = tracker.update(dets);

            // Print confirmed detections
            for (const auto& t : tracked) {
                if (!t.confirmed) continue;
                printf("  [%4d] %-16s %3.0f%%  "
                       "(%5.0f,%5.0f)-(%5.0f,%5.0f)  age=%d\n",
                       t.trackId, className(t.classId), t.score * 100.f,
                       t.x1, t.y1, t.x2, t.y2, t.age);
            }
            if (!tracked.empty())
                printf("  frame=%-6llu  dets=%-3zu  tracks=%zu  "
                       "queue_dropped=%llu\n\n",
                       (unsigned long long)frame.frameId,
                       dets.size(), tracked.size(),
                       (unsigned long long)inferQueue.dropped());

            // Persist
            std::vector<DetectionRecord> records;
            for (const auto& t : tracked) {
                if (!t.confirmed) continue;
                DetectionRecord r{};
                r.frameId     = frame.frameId;
                r.timestampUs = frame.timestampNs / 1000;
                r.trackId     = t.trackId;
                r.classId     = t.classId;
                r.label       = className(t.classId);
                r.x1 = t.x1; r.y1 = t.y1;
                r.x2 = t.x2; r.y2 = t.y2;
                r.confidence  = t.score;
                r.frameWidth  = frame.width;
                r.frameHeight = frame.height;
                records.push_back(r);
            }
            if (!records.empty())
                db.writeBatch(records);

            // Streaming is handled by VENC hardware via JpegCallback — no
            // per-frame CPU work needed here.

            // Crops — full-res NV12 (1920×1080) from VPSS chn2.
            // Detection boxes are already in sensor coordinates; no scaling.
            for (const auto& t : tracked) {
                if (!t.confirmed) continue;
                if (!frame.nv12_fullres.empty())
                    crops.submit(frame.nv12_fullres,
                                 frame.width, frame.height,
                                 t.trackId, t.classId,
                                 className(t.classId),
                                 t.score,
                                 t.x1, t.y1, t.x2, t.y2);
            }
        }
    });

    // ── Wait for RKNN context to be ready ────────────────────────────────────
    {
        std::unique_lock<std::mutex> lk(rknnReadyMtx);
        rknnReadyCV.wait(lk, [&]{ return rknnReady; });
    }
    if (!rknnInitOk) {
        fprintf(stderr, "Fatal: RKNN eager init failed — aborting before RKMPI start\n");
        inferQueue.stop();
        if (inferThread.joinable()) inferThread.join();
        http.close(); sse.close(); streamer.close();
        crops.close(); db.close();
        return 1;
    }
    printf("RKNN NPU context ready — starting RKMPI capture\n\n");

    // ── rkaiq ISP engine initialisation ──────────────────────────────────────
    //
    // rkaiq must be initialised AFTER the RKNN context (NPU DMA allocated above)
    // and BEFORE cam.open() (which calls RK_MPI_SYS_Init() and VI_EnableChn).
    // This ordering matches the official rv1106_aiisp_ipc SDK and ensures that
    // NPU DMA and rkaiq ISP DMA do not collide on the RV1106 AXI bus.
    //
    // With rkaiq running:
    //   • AWB, AE, CCM and gamma correction are performed inside the ISP hardware.
    //   • The NV12 frames from VI are already properly white-balanced.
    //   • Software WB correction (wbR/wbG/wbB in RkmpiConfig) is set to 1.0.
    //
    // On failure (IQ files missing, sensor probe fails, etc.) we fall back
    // gracefully to the empirical software WB correction applied in softwareJpegLoop.
#ifdef HAVE_RKAIQ
    if (flagNoRkaiq) {
        printf("[rkaiq] --no-rkaiq flag set — skipping ISP engine init"
               " (software WB, NPU unconstrained)\n\n");
    } else {
        static const char* IQ_DIR       = "/oem/usr/share/iqfiles";
        // On RV1106, /dev/video0-10 are CIF (rkcif) capture nodes.
        // rkaiq binds to the ISP (rkisp_v7) main path at /dev/video11.
        static const char* VI_VIDEO_DEV = "/dev/video11";

        printf("Initialising rkaiq ISP engine (IQ dir: %s)...\n", IQ_DIR);

        // Resolve the sensor entity name bound to the VI video device.
        // rkaiq needs this to locate the correct IQ tuning file and configure
        // the ISP pipeline for this specific sensor.
        const char* snsEntName =
            rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(VI_VIDEO_DEV);

        if (!snsEntName || snsEntName[0] == '\0') {
            fprintf(stderr, "[rkaiq] cannot resolve sensor entity name for %s"
                            " — falling back to software WB\n", VI_VIDEO_DEV);
        } else {
            printf("  sensor entity : %s\n", snsEntName);

            aiqCtx = rk_aiq_uapi2_sysctl_init(snsEntName, IQ_DIR,
                                               nullptr, nullptr);
            if (!aiqCtx) {
                fprintf(stderr, "[rkaiq] sysctl_init failed"
                                " — falling back to software WB\n");
            } else {
                // rkaiq is prepared at 640×480 (not 1920×1080) to reduce the
                // ISP output DMA from ~93 MB/s to ~14 MB/s and prevent NPU
                // AXI bus starvation.  Must be called after init, before start.
                static const uint32_t AIQ_WIDTH  = 640;
                static const uint32_t AIQ_HEIGHT = 480;
                XCamReturn ret = rk_aiq_uapi2_sysctl_prepare(
                    aiqCtx, AIQ_WIDTH, AIQ_HEIGHT,
                    RK_AIQ_WORKING_MODE_NORMAL);

                if (ret != XCAM_RETURN_NO_ERROR) {
                    fprintf(stderr, "[rkaiq] sysctl_prepare failed (%d)"
                                    " — falling back to software WB\n", ret);
                    rk_aiq_uapi2_sysctl_deinit(aiqCtx);
                    aiqCtx = nullptr;
                } else {
                    ret = rk_aiq_uapi2_sysctl_start(aiqCtx);
                    if (ret != XCAM_RETURN_NO_ERROR) {
                        fprintf(stderr, "[rkaiq] sysctl_start failed (%d)"
                                        " — falling back to software WB\n", ret);
                        rk_aiq_uapi2_sysctl_deinit(aiqCtx);
                        aiqCtx = nullptr;
                    } else {
                        printf("  rkaiq started — AWB/AE/CCM active in ISP hardware\n");
                        // ISP now handles white-balance in hardware: neutralise
                        // the software WB gains so softwareJpegLoop is a no-op.
                        camCfg.wbR = camCfg.wbG = camCfg.wbB = 1.0f;
                        printf("  software WB correction disabled (ISP handles it)\n");
                        // Reduce capture resolution to 640×480 to lower ISP
                        // output DMA from ~93 MB/s (1920×1080 NV12 @30fps) to
                        // ~14 MB/s (640×480 @30fps).  The ISP sensor input path
                        // (MIPI → CIF) is unaffected; only the ISP output write
                        // to DDR is reduced.  rkaiq AE/AWB statistics remain
                        // valid at 640×480.  Crop saves downgrade gracefully.
                        camCfg.captureWidth  = 640;
                        camCfg.captureHeight = 480;
                        printf("  ISP capture resolution reduced to %dx%d"
                               " (ISP output DMA: ~14 MB/s vs 93 MB/s @ 1080p)\n\n",
                               camCfg.captureWidth, camCfg.captureHeight);
                    }
                }
            }
        }
    }
#endif  // HAVE_RKAIQ

    // ── RKMPI capture ─────────────────────────────────────────────────────────
    //
    // Opened AFTER RKNN context is fully initialised to avoid simultaneous
    // DMA allocation on the RV1106 AXI bus (see inference thread comment above).
    //
    // NOTE: rkipc must be stopped before this point.  S99trap-rkmpi calls
    // `/etc/init.d/S21appinit stop` before starting yolo_rkmpi.
    RkmpiCapture cam;

    cam.setErrorCallback([](const std::string& msg) {
        fprintf(stderr, "[camera] %s\n", msg.c_str());
    });

    cam.setCallback([&](const CaptureFrame& frame) {
        inferQueue.push(frame);
    });

    // VENC hardware MJPEG: each encoded frame arrives here from the VENC thread.
    // pushJpeg copies the data into the streamer's frame slot and notifies clients.
    cam.setJpegCallback([&](const uint8_t* data, size_t len) {
        streamer.pushJpeg(data, len);
    });

    try { cam.open(camCfg); }
    catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
        inferQueue.stop();
        if (inferThread.joinable()) inferThread.join();
        http.close(); sse.close(); streamer.close();
        crops.close(); db.close();
        return 1;
    }

    cam.start();
    printf("Running — Ctrl+C to stop\n\n");

    // ── Main stats loop ───────────────────────────────────────────────────────
    auto lastStats    = std::chrono::steady_clock::now();
    uint64_t lastFrameCount = 0;

    while (!g_stop.load() && cam.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= std::chrono::seconds(10)) {
            double elapsed = std::chrono::duration<double>(now - lastStats).count();
            uint64_t fc = g_frameCount;
            currentFps = static_cast<float>((fc - lastFrameCount) / elapsed);
            lastFrameCount = fc;

            cam.printStats();
            printf("  infer_queue_dropped=%llu\n", (unsigned long long)inferQueue.dropped());
            DetectionStats ds = db.getStats();
            printf("  DB rows=%lld  tracks=%lld  size=%.1f MB\n\n",
                   (long long)ds.totalDetections,
                   (long long)ds.uniqueTracks,
                   (double)db.fileSizeBytes() / 1e6);

            lastStats = now;
        }
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    printf("\nShutting down...\n");
    cam.stop();
    inferQueue.stop();
    if (inferThread.joinable()) inferThread.join();

    // Stop rkaiq after VI/RKMPI is closed (cam.stop() deactivates VI first).
#ifdef HAVE_RKAIQ
    if (aiqCtx) {
        printf("Stopping rkaiq ISP engine...\n");
        rk_aiq_uapi2_sysctl_stop(aiqCtx, /*keep_ext_hw_st=*/false);
        rk_aiq_uapi2_sysctl_deinit(aiqCtx);
        aiqCtx = nullptr;
    }
#endif

    http.close();
    sse.close();
    streamer.close();
    crops.flush();
    crops.close();
    db.flush();
    db.close();
    printf("Done.\n");
    return 0;
}
