// ─────────────────────────────────────────────────────────────────────────────
// radio_task.cpp — RadioTask: WiFi + HTTP POST + NVS ring buffer
// ─────────────────────────────────────────────────────────────────────────────

#include "radio_task.h"
#include "pipeline.h"
#include "trap_config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

static const char* TAG = "radio";

RadioTask g_radio_task;

// ── WiFi event group ─────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_events;
static StaticEventGroup_t s_wifi_events_storage;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static constexpr EventBits_t WIFI_FAIL_BIT      = BIT1;

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t id, void* /*data*/)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected — reconnecting");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        esp_wifi_connect(); // keep trying
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "WiFi connected");
    }
}

// ── WiFi init ─────────────────────────────────────────────────────────────────

void RadioTask::wifi_init() {
    s_wifi_events = xEventGroupCreateStatic(&s_wifi_events_storage);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

    wifi_config_t wifi_cfg = {};
    // SSID is max 31 chars + NUL (wifi_cfg.sta.ssid = 32 bytes)
    // Password is max 63 chars + NUL (wifi_cfg.sta.password = 64 bytes)
    // Use memcpy with explicit bounds to avoid format/strncpy truncation warnings
    {
        size_t n = strnlen(g_config.wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        memcpy(wifi_cfg.sta.ssid, g_config.wifi_ssid, n);
        wifi_cfg.sta.ssid[n] = '\0';
    }
    {
        size_t n = strnlen(g_config.wifi_pass, sizeof(wifi_cfg.sta.password) - 1);
        memcpy(wifi_cfg.sta.password, g_config.wifi_pass, n);
        wifi_cfg.sta.password[n] = '\0';
    }
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi STA started, SSID: %s", g_config.wifi_ssid);
}

bool RadioTask::wifi_wait_connected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ── JSON serialisation ────────────────────────────────────────────────────────
// Compact format — fits in a single LoRa payload:
//   {"trap":1,"ts":1743000000,"cls":0,"conf":87,"cnt":3}

int RadioTask::format_json(char* buf, size_t cap, const DetectionEvent& evt) {
    int64_t ts_sec = evt.timestamp_us / 1'000'000LL;
    return snprintf(buf, cap,
        "{\"trap\":%u,\"ts\":%" PRId64 ",\"cls\":%u,\"conf\":%u,\"cnt\":1}",
        evt.trap_id, ts_sec, evt.class_id, evt.confidence);
}

// ── HTTP POST ─────────────────────────────────────────────────────────────────

esp_err_t RadioTask::http_post_event(const DetectionEvent& evt) {
    char body[128];
    format_json(body, sizeof(body), evt);

    esp_http_client_config_t http_cfg = {
        .url             = g_config.server_url,
        .method          = HTTP_METHOD_POST,
        .timeout_ms      = 5000,
        .buffer_size     = 512,
        .buffer_size_tx  = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP %d from server", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "HTTP error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

// ── NVS ring buffer ───────────────────────────────────────────────────────────

void RadioTask::nvs_write_event(const DetectionEvent& evt) {
    nvs_handle_t h;
    if (nvs_open("radio_buf", NVS_READWRITE, &h) != ESP_OK) return;

    uint32_t head = 0;
    nvs_get_u32(h, "head", &head);

    char key[16];
    snprintf(key, sizeof(key), "e%lu", (unsigned long)(head % NVS_RING_SIZE));
    nvs_set_blob(h, key, &evt, sizeof(evt));

    head++;
    nvs_set_u32(h, "head", head);
    nvs_commit(h);
    nvs_close(h);

    buffered_count_++;
    ESP_LOGD(TAG, "Event buffered to NVS (head=%lu)", (unsigned long)head);
}

uint32_t RadioTask::nvs_flush_buffered() {
    nvs_handle_t h;
    if (nvs_open("radio_buf", NVS_READWRITE, &h) != ESP_OK) return 0;

    uint32_t head = 0, tail = 0;
    nvs_get_u32(h, "head", &head);
    nvs_get_u32(h, "tail", &tail);

    uint32_t flushed = 0;
    while (tail != head) {
        char key[16];
        snprintf(key, sizeof(key), "e%lu", (unsigned long)(tail % NVS_RING_SIZE));

        DetectionEvent evt = {};
        size_t sz = sizeof(evt);
        if (nvs_get_blob(h, key, &evt, &sz) == ESP_OK) {
            if (http_post_event(evt) == ESP_OK) {
                nvs_erase_key(h, key);
                tail++;
                flushed++;
                sent_count_++;
            } else {
                break; // still no connectivity — stop trying
            }
        } else {
            tail++; // corrupt entry — skip
        }
    }

    nvs_set_u32(h, "tail", tail);
    nvs_commit(h);
    nvs_close(h);

    if (flushed > 0) {
        ESP_LOGI(TAG, "Flushed %lu buffered events", (unsigned long)flushed);
        buffered_count_ = (buffered_count_ > flushed) ? buffered_count_ - flushed : 0;
    }
    return flushed;
}

// ── RadioTask::run() ──────────────────────────────────────────────────────────

void RadioTask::run() {
    ESP_LOGI(TAG, "Starting radio task (mode=%s)",
             g_config.radio_mode == RadioMode::WiFi ? "WiFi" : "LoRa");

    if (g_config.radio_mode == RadioMode::WiFi) {
        wifi_init();
        // Wait up to 30 s for initial connection — don't block inference
        if (wifi_wait_connected(30000)) {
            connected_ = true;
            nvs_flush_buffered(); // drain anything from previous wake cycle
        }
    } else {
        // LoRa (SX1262 SPI) — stub for Phase 3 implementation
        // See docs/esp32-p4-insect-trap-architecture.md § LoRaWAN
        ESP_LOGW(TAG, "LoRa radio not yet implemented — events will buffer to NVS");
    }

    constexpr uint32_t FLUSH_INTERVAL_MS = 60'000; // flush NVS every 60 s
    uint32_t last_flush_ms = 0;

    while (true) {
        DetectionEvent evt = {};
        bool got_event = g_detection_queue.receive(evt, pdMS_TO_TICKS(5000));

        // Reconnect WiFi if dropped
        if (g_config.radio_mode == RadioMode::WiFi) {
            EventBits_t bits = xEventGroupGetBits(s_wifi_events);
            connected_ = (bits & WIFI_CONNECTED_BIT) != 0;
        }

        if (got_event) {
            if (connected_ && g_config.radio_mode == RadioMode::WiFi) {
                esp_err_t err = http_post_event(evt);
                if (err == ESP_OK) {
                    sent_count_++;
                    ESP_LOGD(TAG, "Event sent: class=%u conf=%u (total sent: %lu)",
                             evt.class_id, evt.confidence, (unsigned long)sent_count_);
                } else {
                    nvs_write_event(evt);
                }
            } else {
                // Not connected — buffer unconditionally
                nvs_write_event(evt);
            }
        }

        // Periodic NVS flush when connected
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (connected_ && (now_ms - last_flush_ms) >= FLUSH_INTERVAL_MS) {
            nvs_flush_buffered();
            last_flush_ms = now_ms;
        }

        // Check shutdown
        if (g_shutdown_requested && g_detection_queue.waiting() == 0) {
            ESP_LOGI(TAG, "Shutdown: queue drained, radio task exiting");
            // Final flush attempt
            if (connected_) nvs_flush_buffered();
            break;
        }
    }
}
