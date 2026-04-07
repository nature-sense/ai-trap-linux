#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// trap_config.h — Runtime configuration backed by NVS
//
// All parameters live in the "trap_cfg" NVS namespace.
// Defaults are compiled in; call trap_config_load() at startup to overlay NVS.
// Call trap_config_save() to persist any change made at runtime.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char* TAG_CFG = "trap_cfg";
static const char* NVS_NS  = "trap_cfg";

// ── Radio mode ───────────────────────────────────────────────────────────────
enum class RadioMode : uint8_t {
    WiFi   = 0,   ///< ESP32-C6 addon via ESP-Hosted
    LoRa   = 1,   ///< SX1262 via SPI
};

// ── Configuration struct ─────────────────────────────────────────────────────
struct TrapConfig {
    // Identity
    uint16_t trap_id         = 1;
    char     location[64]    = "unset";

    // GPS (EXIF / metadata only — not used for navigation)
    double   gps_lat         = 0.0;
    double   gps_lon         = 0.0;
    float    gps_alt_m       = 0.0f;

    // Inference
    float    conf_threshold  = 0.45f;  ///< Detections below this are ignored
    uint8_t  model_class_id  = 0;      ///< Single-class: 0 = insect

    // Camera
    uint32_t capture_fps     = 18;     ///< Target FPS — ESPDet-Pico budget
    uint8_t  pir_gpio        = 5;      ///< GPIO connected to PIR OUT

    // Power
    uint32_t idle_timeout_s  = 30;     ///< No detection for N seconds → deep sleep

    // Radio
    RadioMode radio_mode     = RadioMode::WiFi;

    // WiFi (used when radio_mode == WiFi)
    char wifi_ssid[64]       = "";
    char wifi_pass[64]       = "";
    char server_url[256]     = "http://192.168.1.100:8080/api/detections";

    // LoRa SX1262 (used when radio_mode == LoRa)
    uint8_t  lora_spi_host   = 1;      ///< SPI host (SPI2_HOST=1)
    uint8_t  lora_cs_gpio    = 10;
    uint8_t  lora_irq_gpio   = 11;
    uint8_t  lora_rst_gpio   = 12;
    uint8_t  lora_busy_gpio  = 13;
    uint32_t lora_freq_hz    = 868100000; ///< EU868 band
    char     lora_dev_eui[17]= "0000000000000000";
    char     lora_app_key[33]= "00000000000000000000000000000000";

    // Sampling (1-in-N JPEG capture for QC/retraining — 0 = disabled)
    uint32_t sample_rate     = 0;
};

// ── Load / save ───────────────────────────────────────────────────────────────

/// Load configuration from NVS, overlaying compiled-in defaults.
/// Call once from app_main() after nvs_flash_init().
inline void trap_config_load(TrapConfig& cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_CFG, "No NVS namespace found — using compiled defaults");
        return;
    }
    ESP_ERROR_CHECK(err);

    // Helper macros to reduce boilerplate
    auto load_u16 = [&](const char* key, uint16_t& val) {
        uint16_t tmp; if (nvs_get_u16(h, key, &tmp) == ESP_OK) val = tmp;
    };
    auto load_u32 = [&](const char* key, uint32_t& val) {
        uint32_t tmp; if (nvs_get_u32(h, key, &tmp) == ESP_OK) val = tmp;
    };
    auto load_u8 = [&](const char* key, uint8_t& val) {
        uint8_t tmp; if (nvs_get_u8(h, key, &tmp) == ESP_OK) val = tmp;
    };
    auto load_str = [&](const char* key, char* buf, size_t len) {
        size_t required = len;
        nvs_get_str(h, key, buf, &required);
    };
    auto load_f32 = [&](const char* key, float& val) {
        // NVS has no native float; store as uint32 bitcast
        uint32_t tmp;
        if (nvs_get_u32(h, key, &tmp) == ESP_OK)
            memcpy(&val, &tmp, sizeof(float));
    };

    load_u16("trap_id",       cfg.trap_id);
    load_str("location",      cfg.location,    sizeof(cfg.location));
    load_f32("conf_thresh",   cfg.conf_threshold);
    load_u32("idle_timeout",  cfg.idle_timeout_s);
    load_u8 ("pir_gpio",      cfg.pir_gpio);
    load_u8 ("radio_mode",    reinterpret_cast<uint8_t&>(cfg.radio_mode));
    load_str("wifi_ssid",     cfg.wifi_ssid,   sizeof(cfg.wifi_ssid));
    load_str("wifi_pass",     cfg.wifi_pass,   sizeof(cfg.wifi_pass));
    load_str("server_url",    cfg.server_url,  sizeof(cfg.server_url));
    load_u32("lora_freq",     cfg.lora_freq_hz);
    load_str("lora_dev_eui",  cfg.lora_dev_eui,sizeof(cfg.lora_dev_eui));
    load_str("lora_app_key",  cfg.lora_app_key,sizeof(cfg.lora_app_key));
    load_u32("sample_rate",   cfg.sample_rate);

    nvs_close(h);
    ESP_LOGI(TAG_CFG, "Config loaded: trap_id=%u location=%s", cfg.trap_id, cfg.location);
}

/// Persist the current config to NVS. Call after any runtime config change.
inline esp_err_t trap_config_save(const TrapConfig& cfg) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));

    nvs_set_u16(h, "trap_id",      cfg.trap_id);
    nvs_set_str(h, "location",     cfg.location);
    nvs_set_u8 (h, "radio_mode",   static_cast<uint8_t>(cfg.radio_mode));
    nvs_set_str(h, "wifi_ssid",    cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass",    cfg.wifi_pass);
    nvs_set_str(h, "server_url",   cfg.server_url);
    nvs_set_u32(h, "idle_timeout", cfg.idle_timeout_s);
    nvs_set_u8 (h, "pir_gpio",     cfg.pir_gpio);
    nvs_set_u32(h, "lora_freq",    cfg.lora_freq_hz);
    nvs_set_str(h, "lora_dev_eui", cfg.lora_dev_eui);
    nvs_set_str(h, "lora_app_key", cfg.lora_app_key);
    nvs_set_u32(h, "sample_rate",  cfg.sample_rate);

    uint32_t f_bits; memcpy(&f_bits, &cfg.conf_threshold, 4);
    nvs_set_u32(h, "conf_thresh", f_bits);

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/// Global singleton config — populated at startup, read-only thereafter.
/// Tasks access this directly; RadioTask may update server_url at runtime.
extern TrapConfig g_config;
