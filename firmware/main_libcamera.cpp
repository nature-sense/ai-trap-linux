#include "libcamera_capture.h"
#include "decoder.h"
#include "tracker.h"
#include "persistence.h"
#include "crop_saver.h"
#include "mjpeg_streamer.h"
#include "sse_server.h"
#include "http_server.h"
#include "sync_manager.h"
#include "trap_events.h"
#include "config_loader.h"
#include "wifi_manager.h"
#include "ble_gatt_server.h"
#include "epaper_display.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Class names
// ─────────────────────────────────────────────────────────────────────────────

static const char* CLASS_NAMES[1] = { "insect" };
static const char* className(int id) { return id == 0 ? CLASS_NAMES[0] : "?"; }

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_stopSignal{0};
static void onSignal(int sig) { g_stopSignal = sig; g_stop = true; }

// ─────────────────────────────────────────────────────────────────────────────
//  Session ID helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string makeSessionId() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
//
//  Usage: ./yolo_libcamera [config.toml]
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* configPath = argc > 1 ? argv[1] : "trap_config.toml";

    printf("═══════════════════════════════════════════════\n");
    printf("  YOLO11n  Pi 5  libcamera + ByteTracker\n");
    printf("═══════════════════════════════════════════════\n");
    printf("  config : %s\n\n", configPath);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);  // prevent exit on client disconnect

    // ── Load configuration ────────────────────────────────────────────────────

    TrapConfig cfg;
    if (!loadConfig(configPath, cfg))
        fprintf(stderr, "Warning: config file not found — using built-in defaults\n\n");
    printConfig(cfg);

    // ── Components ────────────────────────────────────────────────────────────

    SqliteWriter  db;
    YoloDecoder   decoder(cfg.decoder);
    ByteTracker   tracker(cfg.tracker);
    CropSaver     crops;
    MjpegStreamer streamer;
    SseServer     sse;
    HttpServer    http;
    SyncManager   sync;

    // ── Database ──────────────────────────────────────────────────────────────

    try {
        db.open(cfg.dbPath.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open database: %s\n", e.what());
        return 1;
    }

    // ── Sync manager ─────────────────────────────────────────────────────────

    sync.init(db.rawDb(), cfg.crops.outputDir);

    // ── Crop saver ────────────────────────────────────────────────────────────

    try {
        crops.open(cfg.crops);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot open crop saver: %s\n", e.what());
        db.close();
        return 1;
    }

    // ── MJPEG streamer ────────────────────────────────────────────────────────

    try {
        streamer.open(cfg.stream);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot start MJPEG streamer: %s\n", e.what());
        crops.close();
        db.close();
        return 1;
    }

    // ── SSE server ────────────────────────────────────────────────────────────

    try {
        sse.open(cfg.sse);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot start SSE server: %s\n", e.what());
        streamer.close();
        crops.close();
        db.close();
        return 1;
    }

    // ── HTTP API server ───────────────────────────────────────────────────────

    // fps is updated by the main stats loop; the HTTP server reads it on demand.
    float currentFps = 0.f;

    // Capture flag — true means inference/tracking/saving is active.
    // The MJPEG stream always runs regardless of this flag.
    std::atomic<bool> g_capturing{true};

    // Current capture session ID (empty when not capturing).
    // Protected by sessionMutex — written from the HTTP thread, read from
    // the HTTP thread and the saved-crop callback.
    std::string currentSessionId;
    std::mutex  sessionMutex;

    // Start a new capture session: picks a timestamped ID, creates the
    // subdirectory, and notifies CropSaver and SyncManager.
    auto startCaptureSession = [&]() {
        std::string sid = makeSessionId();
        std::string sessionDir = cfg.crops.outputDir + "/" + sid;
        crops.startSession(sessionDir);
        sync.setCurrentSession(sid);
        {
            std::lock_guard<std::mutex> lk(sessionMutex);
            currentSessionId = sid;
        }
        printf("main: capture session started: %s\n", sid.c_str());
    };

    http.setLocationCallback([&](double lat, double lon) {
        cfg.gpsLat   = lat;
        cfg.gpsLon   = lon;
        cfg.gpsValid = true;
        // Live-update CropSaver EXIF template so new crops get GPS immediately
        auto& ep  = cfg.crops.exifTemplate;
        ep.lat    = lat;
        ep.lon    = lon;
        ep.hasGps = true;
        printf("main: GPS updated  lat=%.6f  lon=%.6f\n", lat, lon);
    });

    http.setThresholdCallback([&](float thresh) {
        DecoderConfig dc = decoder.config();
        dc.confThresh = thresh;
        decoder.setConfig(dc);
    });

    http.setCaptureCallback([&](bool active) {
        if (active) {
            startCaptureSession();
            g_capturing = true;
        } else {
            g_capturing = false;
            sync.setCurrentSession("");
            {
                std::lock_guard<std::mutex> lk(sessionMutex);
                currentSessionId = "";
            }
            printf("main: capture session closed\n");
        }
        sse.pushEvent(TrapEvents::captureState(active));
    });

    // Populate EXIF template from TrapConfig (GPS, trapId, location)
    {
        auto& ep          = cfg.crops.exifTemplate;
        ep.trapId         = cfg.trapId;
        ep.trapLocation   = cfg.trapLocation;
        ep.hasGps         = cfg.gpsValid;
        ep.lat            = cfg.gpsLat;
        ep.lon            = cfg.gpsLon;
        ep.altM           = cfg.gpsAltM;
    }

    crops.setSavedCallback([&](int trackId, int classId,
                                  const std::string& cls,
                                  float conf, const std::string& path,
                                  int w, int h, int64_t timestampUs) {
        // Extract just the filename (basename) from the full path for SSE
        std::string filename = path;
        auto slash = path.rfind('/');
        if (slash != std::string::npos) filename = path.substr(slash + 1);

        sse.pushEvent(TrapEvents::cropSaved(
            trackId, cls.c_str(), conf, filename.c_str(), w, h));

        // Register in the DB so the sync API can serve it.
        // File size is read from disk after the write has completed.
        struct stat st{};
        int64_t bytes = (::stat(path.c_str(), &st) == 0)
                        ? static_cast<int64_t>(st.st_size) : 0;

        // TODO: replace stubs with real BME280/BME680 sensor readings.
        // NaN causes NULL to be stored in the DB (sensor not fitted).
        const float kNaN = std::numeric_limits<float>::quiet_NaN();
        float tempC  = kNaN;
        float humPct = kNaN;
        float presHpa= kNaN;

        sync.registerCrop(filename, trackId, classId, cls, conf, timestampUs, bytes,
                          tempC, humPct, presHpa);
    });

    http.setAfTriggerCallback([&]() {
        // Trigger a one-shot AF scan.  LibcameraCapture will pick this up
        // on the next call to applyControls() — add that method if needed.
        printf("AF trigger requested via API\n");
    });

    http.setSessionIdCallback([&]() -> std::string {
        std::lock_guard<std::mutex> lk(sessionMutex);
        return currentSessionId;
    });

    // ── WiFi + BLE management ─────────────────────────────────────────────────
    // Skipped when wifi.managed = false in trap_config.toml (e.g. dev Pi that
    // stays on a known network — OS handles WiFi, BLE stays idle).

    WifiManager  wifi(cfg.trapId, cfg.wifi);
    BleGattServer ble;

    if (cfg.wifi.managed) {
        // Starts in AP mode on first boot (no creds file).
        // POST /api/wifi switches to station; POST /api/wifi/reset returns to AP.
        wifi.applyStartupMode();
        http.setWifiManager(&wifi);

        // Pure C++ BLE GATT server — no Python, no D-Bus.
        // Note: bluetoothd must be stopped (done by install.sh).
        BleGattConfig bleCfg;
        bleCfg.name   = "AI-Trap-" + cfg.trapId;
        bleCfg.hciDev = 0;
        ble.open(bleCfg, &wifi);

        // Notify BLE client whenever WiFi state changes (inactivity shutdown etc.)
        wifi.setInactiveCallback([&]() { ble.notifyStateChanged(); });
    } else {
        printf("WiFi/BLE management disabled (wifi.managed = false) — using OS WiFi.\n");
    }

    // ── e-Paper display ───────────────────────────────────────────────────────
    EpaperDisplay disp;
    if (cfg.display.enabled) {
        if (disp.open(cfg.display))
            disp.showLoading(cfg.trapId);
        else
            fprintf(stderr, "[epaper] display unavailable — continuing without\n");
    }

    try {
        http.open(cfg.http, &db, &sse, &sync, &currentFps, &g_capturing);
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: cannot start HTTP server: %s\n", e.what());
        sse.close();
        streamer.close();
        crops.close();
        db.close();
        return 1;
    }

    // ── Initial capture session ───────────────────────────────────────────────
    // Capture starts as active on boot — create the first session now so
    // crops are immediately written into a timestamped subdirectory.
    startCaptureSession();

    // ── LibcameraCapture ──────────────────────────────────────────────────────

    LibcameraCapture cam;

    cam.setErrorCallback([](const std::string& msg) {
        fprintf(stderr, "[camera] %s\n", msg.c_str());
    });

    // Track last AF state to include in health events
    std::atomic<int>   lastAfState{0};
    std::atomic<float> lastLensPos{0.f};

    // ── Per-frame callback ────────────────────────────────────────────────────

    cam.setCallback([&](const CaptureFrame& frame) {

        lastAfState.store(frame.afState,      std::memory_order_relaxed);
        lastLensPos.store(frame.lensPosition, std::memory_order_relaxed);

        // MJPEG stream always runs regardless of capture state
        streamer.pushFrame(frame.nv12, frame.width, frame.height);

        // Inference/tracking/saving only when capturing
        if (!g_capturing.load(std::memory_order_relaxed)) return;

        // Inference is RKNN-only — not available on this target
        std::vector<Detection> dets;

        // Track
        std::vector<TrackedObject> tracked = tracker.update(dets);

        // Per-track actions
        for (const auto& t : tracked) {
            if (!t.confirmed) continue;

            // Log and fire SSE only on first confirmation
            if (t.age == cfg.tracker.minHits) {
                printf("  [%4d] %-16s %3.0f%%  "
                       "(%5.0f,%5.0f)-(%5.0f,%5.0f)  frame=%llu\n",
                       t.trackId, className(t.classId), t.score * 100.f,
                       t.x1, t.y1, t.x2, t.y2,
                       (unsigned long long)frame.frameId);

                sse.pushEvent(TrapEvents::detection(
                    t.trackId, className(t.classId), t.score,
                    t.x1, t.y1, t.x2, t.y2, frame.frameId));
            }
        }

        // Persist to database — write once on first confirmation only.
        // Stationary tracks do not generate a row on every frame.
        {
            std::vector<DetectionRecord> records;
            for (const auto& t : tracked) {
                if (!t.confirmed) continue;
                if (t.age != cfg.tracker.minHits) continue;  // first confirmation only
                DetectionRecord r{};
                r.frameId     = frame.frameId;
                r.timestampUs = frame.timestampNs / 1000;
                r.trackId     = t.trackId;
                r.classId     = t.classId;
                r.label       = className(t.classId);
                r.x1          = t.x1; r.y1 = t.y1;
                r.x2          = t.x2; r.y2 = t.y2;
                r.confidence  = t.score;
                r.frameWidth  = frame.width;
                r.frameHeight = frame.height;
                records.push_back(r);
            }
            if (!records.empty())
                db.writeBatch(records);
        }

        // Save best-confidence JPEG crop per confirmed track
        for (const auto& t : tracked) {
            if (!t.confirmed) continue;
            crops.submit(frame.nv12,
                         frame.width, frame.height,
                         t.trackId, t.classId,
                         className(t.classId),
                         t.score,
                         t.x1, t.y1, t.x2, t.y2,
                         static_cast<int64_t>(frame.timestampNs / 1000));
        }
    });

    // ── Open and start camera ─────────────────────────────────────────────────

    try {
        cam.open(cfg.camera);
        cam.start();
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
        http.close(); sse.close(); streamer.close(); crops.close(); db.close();
        return 1;
    }

    printf("Running — Ctrl+C to stop\n");
    printf("  MJPEG stream : http://192.168.5.1:%d/stream\n", cfg.stream.port);
    printf("  SSE events   : http://192.168.5.1:%d/api/events\n", cfg.sse.port);
    printf("  REST API     : http://192.168.5.1:%d/api/status\n\n", cfg.http.port);

    // ── Main loop — stats every 30 s ─────────────────────────────────────────

    auto lastStats  = std::chrono::steady_clock::now();
    auto startTime  = lastStats;
    uint64_t lastFrameId = 0;

    while (!g_stop.load() && cam.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= std::chrono::seconds(30)) {
            double elapsed = std::chrono::duration<double>(now - lastStats).count();

            cam.printStats();

            DetectionStats ds = db.getStats();
            double dbMb = static_cast<double>(db.fileSizeBytes()) / 1e6;
            long long uptimeS = std::chrono::duration_cast<std::chrono::seconds>(
                                    now - startTime).count();

            printf("  DB rows=%lld  tracks=%lld  size=%.1f MB\n\n",
                   (long long)ds.totalDetections,
                   (long long)ds.uniqueTracks,
                   dbMb);
            crops.printStats();
            streamer.printStats();
            sse.printStats();
            http.printStats();

            // Push SSE stats + health events
            sse.pushEvent(TrapEvents::stats(
                ds.totalDetections, uptimeS, currentFps,
                ds.uniqueTracks, dbMb));

            sse.pushEvent(TrapEvents::health(
                TrapEvents::readCpuTemp(),
                lastAfState.load(std::memory_order_relaxed),
                lastLensPos.load(std::memory_order_relaxed)));

            // Update e-paper display
            if (disp.isOpen()) {
                auto t = std::chrono::system_clock::to_time_t(
                             std::chrono::system_clock::now());
                struct tm tm_buf{};
                localtime_r(&t, &tm_buf);
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm_buf);

                EpaperDisplay::Content dc;
                dc.trapId     = cfg.trapId;
                dc.detections = ds.totalDetections;
                dc.tracks     = ds.uniqueTracks;
                dc.uptimeSecs = uptimeS;
                dc.timeStr    = tbuf;
                if (cfg.wifi.managed) {
                    auto ws   = wifi.getStatus();
                    dc.ip     = ws.ip;
                    dc.wifiMode = ws.mode;
                }
                disp.update(dc);
            }

            lastStats = now;
            (void)elapsed;
        }
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────

    int sig = g_stopSignal.load();
    if (sig)
        printf("\nCaught signal %d (%s) — shutting down...\n", sig, strsignal(sig));
    else
        printf("\nCamera stopped unexpectedly — shutting down...\n");
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
