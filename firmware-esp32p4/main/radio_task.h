#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// radio_task.h — RadioTask: transmit detection events over WiFi (HTTP POST)
//
// Responsibilities:
//   • Maintain WiFi STA connection (reconnect on drop)
//   • Consume DetectionEvents from g_detection_queue
//   • Serialize as compact JSON and POST to server_url
//   • On failure: write event to NVS ring buffer for retry
//   • Periodically flush NVS buffer when connection is restored
//   • Aggregate: coalesce rapid detections (same class, <2 s apart) into counts
//
// Core pinning: Core 0 (tskNO_AFFINITY preferred — shares with CameraTask),
//               priority 3 (below InferenceTask — radio never stalls inference)
// Stack: 8 KB
//
// LoRa option: RadioMode::LoRa selects SX1262 SPI path instead of WiFi.
//   Both paths share the same NVS buffering and retry logic.
// ─────────────────────────────────────────────────────────────────────────────

#include "pipeline.h"
#include "trap_config.h"

class RadioTask : public Task<8192> {
public:
    RadioTask() = default;
    void begin() { start("radio", 3, tskNO_AFFINITY); }

    bool is_connected() const { return connected_; }
    uint32_t events_sent()   const { return sent_count_; }
    uint32_t events_buffered() const { return buffered_count_; }

protected:
    void run() override;

private:
    volatile bool     connected_      = false;
    volatile uint32_t sent_count_     = 0;
    volatile uint32_t buffered_count_ = 0;

    // ── WiFi path ─────────────────────────────────────────────────────────
    void wifi_init();
    bool wifi_wait_connected(uint32_t timeout_ms);
    esp_err_t http_post_event(const DetectionEvent& evt);

    // ── NVS ring buffer ───────────────────────────────────────────────────
    // Key schema: "evt_<index>"  — indices wrap at NVS_RING_SIZE
    // "evt_head" = next write index,  "evt_tail" = next read index
    static constexpr uint32_t NVS_RING_SIZE = 256; // events

    void nvs_write_event(const DetectionEvent& evt);
    uint32_t nvs_flush_buffered();   // returns number flushed

    // ── JSON serialisation ────────────────────────────────────────────────
    // Format: {"trap":1,"ts":1743000000,"cls":0,"conf":87,"cnt":1}
    // ~55 bytes per event — fits in a single LoRa payload alongside metadata.
    static int format_json(char* buf, size_t cap, const DetectionEvent& evt);

    // ── Aggregation: merge rapid repeated detections ──────────────────────
    // If the same class arrives again within COALESCE_US, increment counter
    // and defer transmission until the burst ends or timeout expires.
    static constexpr int64_t COALESCE_US = 2'000'000; // 2 seconds

    int64_t  last_ts_us_   = 0;
    uint8_t  last_class_   = 0xFF;
    uint32_t burst_count_  = 0;
};

extern RadioTask g_radio_task;
