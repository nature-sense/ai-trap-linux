#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ov5647.h — OV5647 sensor initialisation over I2C (SCCB)
//
// The OV5647 is a 5 MP CMOS sensor used in the Raspberry Pi Camera Module v1.
// It supports both DVP parallel and MIPI CSI-2 (1-lane) interfaces.
// Here we use MIPI CSI-2 1-lane to feed the ESP32-P4 CSI controller.
//
// Capture configuration: 640×480 RAW10 MIPI, ~30 fps
// The P4 ISP crops/scales the output to FRAME_W × FRAME_H (224×224) YUV420
// before DMA transfer, matching the ESPDet-Pico input resolution exactly.
//
// Wiring assumptions (Stamp-P4 + OV5647 module):
//   MIPI D0+/D0-  → CSI lane 0 diff pair
//   MIPI CLK+/CLK-→ CSI clock pair
//   SCL            → GPIO 8  (configurable in TrapConfig / here)
//   SDA            → GPIO 9  (configurable)
//   PWDN           → GPIO 6  (active high = power down)
//   XCLK           → LEDC channel / XTAL output (typically not needed for OV5647 MIPI)
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include "driver/i2c_master.h"
#include "esp_err.h"

static constexpr uint8_t  OV5647_I2C_ADDR    = 0x36; ///< 7-bit SCCB address
static constexpr uint32_t OV5647_I2C_FREQ_HZ = 100000;
static constexpr uint8_t  OV5647_I2C_SCL_GPIO = 8;
static constexpr uint8_t  OV5647_I2C_SDA_GPIO = 9;
static constexpr uint8_t  OV5647_PWDN_GPIO    = 6;

/// Sentinel marker for the end of a register table.
static constexpr uint16_t OV5647_REG_END = 0xFFFF;

/// Register/value pair for sensor init tables.
struct OV5647Reg {
    uint16_t addr;
    uint8_t  val;
};

// ── Public API ────────────────────────────────────────────────────────────────

/// Initialise I2C master bus and power on the OV5647.
/// Must be called before ov5647_configure().
esp_err_t ov5647_init(i2c_master_bus_handle_t* out_bus);

/// Write the 640×480 RAW10 MIPI 1-lane register table to the sensor.
/// The sensor will begin streaming after this call.
esp_err_t ov5647_configure(i2c_master_bus_handle_t bus);

/// Read sensor chip ID registers (0x300A:0x300B) and verify = 0x5647.
/// Returns ESP_ERR_NOT_FOUND if the ID does not match.
esp_err_t ov5647_check_id(i2c_master_bus_handle_t bus);

/// Put the sensor into software standby (stop streaming, preserve config).
esp_err_t ov5647_standby(i2c_master_bus_handle_t bus);

/// Resume streaming from software standby.
esp_err_t ov5647_stream_on(i2c_master_bus_handle_t bus);

/// Hard power-down via PWDN GPIO (lowest power state).
void ov5647_power_down();

/// Power up from hard power-down.
void ov5647_power_up();
