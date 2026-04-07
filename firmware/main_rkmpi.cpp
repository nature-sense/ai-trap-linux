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
    const char* rknnFile = argc > 1 ? argv[1] : "model.rknn";
    const char* dbPath   = argc > 2 ? argv[2] : "detections.db";
    const char* cropsDir = argc > 3 ? argv[3] : "crops";

    printf("══════════════════════════════════════════════════════════\n");
    printf("  YOLO11n  Luckfox Pico Zero  IMX415 RKMPI+VPSS + RKNN NPU\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  model : %s\n  db    : %s\n  crops : %s\n\n",
           rknnFile, dbPath, cropsDir);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

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
    // Step 2 will replace the software JPEG encode here with RKMPI VENC.
    // For now the streamer receives 640×480 NV12 from VPSS channel 1
    // (via CaptureFrame.nv12) instead of the full 1920×1080 NV12 it
    // received in the V4L2 build — reducing software encode cost ~9×.
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

    // ── RKNN model ────────────────────────────────────────────────────────────
    RknnInference net;
    if (!net.init(rknnFile, decCfg.modelWidth, decCfg.modelHeight)) {
        db.close(); return 1;
    }
    printf("RKNN model ready (NPU context deferred to inference thread)\n\n");

    // ── RKMPI capture ─────────────────────────────────────────────────────────
    //
    // NOTE: rkipc must be stopped before this point.  S99trap should call
    // `/etc/init.d/S21appinit stop` (which stops rkipc) before starting
    // yolo_rkmpi.  The ISP runs with static defaults from the device tree
    // — same colour quality as the V4L2 build (green tint without AWB).
    //
    // TODO: Load IQ files via rk_aiq without starting rkaiq_3A_server to
    //       restore AWB/CCM without triggering the NPU AXI DMA conflict.
    RkmpiCapture cam;

    cam.setErrorCallback([](const std::string& msg) {
        fprintf(stderr, "[camera] %s\n", msg.c_str());
    });

    // Enqueue frames from the capture dispatch thread into the inference queue.
    // The capture dispatch thread does NV12→RGB(320×320) + float conversion
    // (~0.6 ms) then returns immediately — it does NOT call rknn_run.
    cam.setCallback([&](const CaptureFrame& frame) {
        inferQueue.push(frame);
    });

    RkmpiConfig camCfg;
    camCfg.captureWidth  = 1920;
    camCfg.captureHeight = 1080;
    camCfg.framerate     = 30;
    camCfg.bufferCount   = 4;
    camCfg.modelWidth    = 320;
    camCfg.modelHeight   = 320;
    camCfg.streamWidth   = 640;
    camCfg.streamHeight  = 480;

    try { cam.open(camCfg); }
    catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
        db.close(); return 1;
    }

    // ── Inference thread ──────────────────────────────────────────────────────
    //
    // Runs rknn_run independently of the capture path.
    // While the NPU processes frame N (~15 ms), the capture dispatch thread
    // is preprocessing frame N+1 (~0.6 ms) — true CPU/NPU overlap.
    //
    // Threading contract (librknnmrt same-thread requirement):
    //   RknnInference::lazyInitCtx() is called on the first infer() here,
    //   so the RKNN context lives entirely in this thread.
    std::thread inferThread([&]() {
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

            // Stream — 640×480 NV12 from VPSS channel 1.
            // Step 2 will replace this with RKMPI VENC hardware MJPEG.
            if (!frame.nv12.empty())
                streamer.pushFrame(frame.nv12, camCfg.streamWidth, camCfg.streamHeight);

            // Crops — use 640×480 NV12 for now (full-res path in Step 2).
            for (const auto& t : tracked) {
                if (!t.confirmed) continue;
                // Scale detection box from sensor coords to stream coords
                const float sx = static_cast<float>(camCfg.streamWidth)  / frame.width;
                const float sy = static_cast<float>(camCfg.streamHeight) / frame.height;
                if (!frame.nv12.empty())
                    crops.submit(frame.nv12,
                                 camCfg.streamWidth, camCfg.streamHeight,
                                 t.trackId, t.classId,
                                 className(t.classId),
                                 t.score,
                                 t.x1 * sx, t.y1 * sy,
                                 t.x2 * sx, t.y2 * sy);
            }
        }
    });

    // ── Start capture ─────────────────────────────────────────────────────────
    cam.start();
    printf("Running — Ctrl+C to stop\n\n");

    // ── Main stats loop ───────────────────────────────────────────────────────
    auto lastStats    = std::chrono::steady_clock::now();
    auto startTime    = lastStats;
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
