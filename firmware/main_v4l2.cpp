#include "v4l2_capture.h"
#include "decoder.h"
#include "tracker.h"
#include "persistence.h"
#include "crop_saver.h"
#include "mjpeg_streamer.h"
#include "sse_server.h"
#include "http_server.h"
#include "sync_manager.h"
#include "wifi_manager.h"
#include "epaper_display.h"
#include "rknn_infer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
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
//  main
//
//  Usage: ./yolo_v4l2 [model.rknn] [db] [crops-dir] [device]
//    model.rknn   RKNN NPU model file   (default model.rknn)
//    db           SQLite output path    (default detections.db)
//    crops-dir    directory for crops   (default crops)
//    device       V4L2 device node      (default /dev/video0)
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* rknnFile = argc > 1 ? argv[1] : "model.rknn";
    const char* dbPath   = argc > 2 ? argv[2] : "detections.db";
    const char* cropsDir = argc > 3 ? argv[3] : "crops";
    const char* device   = argc > 4 ? argv[4] : "/dev/video0";

    printf("══════════════════════════════════════════════════════════\n");
    printf("  YOLO11n  Luckfox Pico Zero  IMX415 V4L2 + RKNN NPU + ByteTracker\n");
    printf("══════════════════════════════════════════════════════════\n");
    printf("  model  : %s\n  db     : %s\n"
           "  crops  : %s\n  device : %s\n\n",
           rknnFile, dbPath, cropsDir, device);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);  // prevent exit on client disconnect

    // ── Components ────────────────────────────────────────────────────────────

    SqliteWriter db;

    DecoderConfig decCfg;
    decCfg.numClasses  = 1;
    decCfg.confThresh  = 0.45f;
    decCfg.nmsThresh   = 0.45f;
    decCfg.modelWidth  = 320;
    decCfg.modelHeight = 320;
    YoloDecoder decoder(decCfg);

    ByteTracker tracker;

    // ── Database ──────────────────────────────────────────────────────────────
    try {
        db.open(dbPath);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open database: %s\n", e.what());
        return 1;
    }

    // ── Crop saver ────────────────────────────────────────────────────────────
    CropSaver crops;
    CropSaverConfig cropCfg;
    cropCfg.outputDir     = cropsDir;
    cropCfg.jpegQuality   = 90;
    cropCfg.minConfidence = 0.50f;
    cropCfg.maxQueueDepth = 16;

    try {
        crops.open(cropCfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open crop saver: %s\n", e.what());
        db.close();
        return 1;
    }

    // ── MJPEG streamer ────────────────────────────────────────────────────────
    MjpegStreamer streamer;
    MjpegStreamerConfig streamCfg;
    streamCfg.port         = 9000;
    streamCfg.streamWidth  = 640;
    streamCfg.streamHeight = 480;
    streamCfg.jpegQuality  = 75;

    try {
        streamer.open(streamCfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot start streamer: %s\n", e.what());
        crops.close();
        db.close();
        return 1;
    }

    // ── SSE server ────────────────────────────────────────────────────────────
    SseServer sse;
    SseConfig sseCfg;
    sseCfg.port = 8081;
    try {
        sse.open(sseCfg);
    } catch (const std::exception& e) {
        fprintf(stderr, "Warning: cannot start SSE server: %s\n", e.what());
        // non-fatal — continue without SSE
    }

    // ── Sync manager ──────────────────────────────────────────────────────────
    SyncManager sync;
    sync.init(db.rawDb(), cropCfg.outputDir);

    // ── HTTP API server ───────────────────────────────────────────────────────
    float currentFps = 0.f;
    uint64_t g_frameCount = 0;   // incremented per inference, for fps calculation
    std::atomic<bool> g_capturing{true};

    HttpServer    http;
    HttpServerConfig httpCfg;
    httpCfg.port     = 8080;
    httpCfg.cropsDir = cropCfg.outputDir;
    httpCfg.trapId   = "luckfox_001";

    // ── WiFi manager ──────────────────────────────────────────────────────────
    WifiConfig wifiCfg;
    wifiCfg.credsPath = "/opt/trap/wifi_creds.conf";
    WifiManager wifi(httpCfg.trapId, wifiCfg);
    wifi.applyStartupMode();
    http.setWifiManager(&wifi);

    // ── e-Paper display ───────────────────────────────────────────────────────
    EpaperDisplay::Config dispCfg;
    // dispCfg.enabled    = true;
    // dispCfg.spiDev     = "/dev/spidev0.0";
    // dispCfg.pinDc      = 49;   // GPIO1_C1 on Luckfox — adjust to your wiring
    // dispCfg.pinRst     = 50;
    // dispCfg.pinBusy    = 51;
    EpaperDisplay disp;
    if (dispCfg.enabled && disp.open(dispCfg))
        disp.showLoading(httpCfg.trapId);

    try {
        http.open(httpCfg, &db, &sse, &sync, &currentFps, &g_capturing);
    } catch (const std::exception& e) {
        fprintf(stderr, "Warning: cannot start HTTP server: %s\n", e.what());
        // non-fatal — continue without REST API
    }

    // ── RKNN model ────────────────────────────────────────────────────────────
    RknnInference net;
    if (!net.init(rknnFile, decCfg.modelWidth, decCfg.modelHeight)) {
        db.close();
        return 1;
    }
    printf("RKNN model ready: %s  (NPU context deferred to inference thread)\n\n", rknnFile);

    // ── V4L2Capture — Luckfox IMX415-98 IR-CUT Camera ────────────────────────
    //
    // IMX415 sensor modes on RV1106 (Luckfox Pico Zero):
    //   3864 x 2192  @  7 fps  — full 4K (ISP bandwidth limited)
    //   1920 x 1080  @ 30 fps  — 1080p  ← recommended
    //   1280 x  720  @ 60 fps  — high framerate
    //
    // Setup (once on board):
    //   v4l2-ctl -d /dev/video0 --list-formats-ext   # verify NV12 offered
    //   media-ctl --print-topology                   # confirm rkisp_mainpath

    V4L2Config camCfg;
    camCfg.device        = device;
    camCfg.captureWidth  = 1920;
    camCfg.captureHeight = 1080;
    camCfg.framerate     = 30;
    camCfg.bufferCount   = 4;
    camCfg.modelWidth    = 320;
    camCfg.modelHeight   = 320;

    V4L2Capture cam;

    cam.setErrorCallback([](const std::string& msg) {
        fprintf(stderr, "[camera] %s\n", msg.c_str());
    });

    // ── Per-frame callback ────────────────────────────────────────────────────

    cam.setCallback([&](const CaptureFrame& frame) {

        // Inference — RKNN NPU
        FloatMat output;
        if (!net.infer(frame.modelInput.data(), output)) {
            fprintf(stderr, "[warn] rknn infer failed\n");
            return;
        }

        // Decode
        std::vector<Detection> dets = decoder.decode(
            output,
            frame.width, frame.height,
            frame.scale, frame.padLeft, frame.padTop);

        // Update fps counter
        g_frameCount++;

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
            printf("  frame=%-6llu  dets=%-3zu  tracks=%zu\n\n",
                   (unsigned long long)frame.frameId,
                   dets.size(), tracked.size());

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

        // Stream
        streamer.pushFrame(frame.nv12, frame.width, frame.height);

        // Crops
        for (const auto& t : tracked) {
            if (!t.confirmed) continue;
            crops.submit(frame.nv12,
                         frame.width, frame.height,
                         t.trackId, t.classId,
                         className(t.classId),
                         t.score,
                         t.x1, t.y1, t.x2, t.y2);
        }
    });

    // ── Open and start ────────────────────────────────────────────────────────
    try {
        cam.open(camCfg);
        cam.start();
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
        db.close();
        return 1;
    }

    printf("Running — Ctrl+C to stop\n\n");

    // ── Main loop ─────────────────────────────────────────────────────────────
    auto lastStats    = std::chrono::steady_clock::now();
    auto startTime    = lastStats;
    uint64_t lastFrameCount = 0;

    while (!g_stop.load() && cam.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= std::chrono::seconds(10)) {
            // Update fps for HTTP API
            double elapsed = std::chrono::duration<double>(now - lastStats).count();
            uint64_t fc = g_frameCount;
            currentFps = static_cast<float>((fc - lastFrameCount) / elapsed);
            lastFrameCount = fc;

            cam.printStats();
            DetectionStats ds = db.getStats();
            printf("  DB rows=%lld  tracks=%lld  size=%.1f MB\n\n",
                   (long long)ds.totalDetections,
                   (long long)ds.uniqueTracks,
                   (double)db.fileSizeBytes() / 1e6);
            crops.printStats();
            streamer.printStats();

            if (disp.isOpen()) {
                auto t = std::chrono::system_clock::to_time_t(
                             std::chrono::system_clock::now());
                struct tm tm_buf{};
                localtime_r(&t, &tm_buf);
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm_buf);

                EpaperDisplay::Content dc;
                dc.trapId     = "luckfox_001";
                dc.detections = ds.totalDetections;
                dc.tracks     = ds.uniqueTracks;
                dc.uptimeSecs = std::chrono::duration_cast<std::chrono::seconds>(
                                    now - startTime).count();
                dc.timeStr    = tbuf;
                auto ws       = wifi.getStatus();
                dc.ip         = ws.ip;
                dc.wifiMode   = ws.mode;
                disp.update(dc);
            }

            lastStats = now;
        }
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    printf("\nShutting down...\n");
    cam.stop();
    disp.close();
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
