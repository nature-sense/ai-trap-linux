// ─────────────────────────────────────────────────────────────────────────────
// ov5647.cpp — OV5647 sensor init: I2C/SCCB + MIPI 1-lane 640×480 RAW10
// ─────────────────────────────────────────────────────────────────────────────

#include "ov5647.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "ov5647";

// ── Register table: 640×480 RAW10 MIPI 1-lane ~30 fps ────────────────────────
//
// Derived from the open-source OV5647 Linux kernel driver and the Raspberry Pi
// camera firmware. Key settings:
//   MIPI: 1 data lane, 200 Mbps lane rate
//   Output: 640×480, RAW10, binned 4x from 2592×1944 full frame
//   Timing: 1896 HTS, 984 VTS → ~30 fps
//
// The P4 ISP receives the RAW10 MIPI stream and produces YUV420 at the
// configured ISP output size (224×224 after crop/scale).
//
static const OV5647Reg ov5647_640x480_mipi[] = {
    // ── Global reset ─────────────────────────────────────────────
    {0x0103, 0x01},  // software reset

    // ── Clock / PLL ───────────────────────────────────────────────
    {0x3034, 0x1a},  // MIPI 10-bit, PLL divider
    {0x3035, 0x21},  // PLL pre-divider = 1, system clock divider = 2
    {0x3036, 0x46},  // PLL multiplier = 70  → VCO ≈ 700 MHz
    {0x303c, 0x11},  // PLL divider options

    // ── MIPI interface ────────────────────────────────────────────
    {0x3821, 0x07},  // horizontal mirror on, ISP mirror on, binning on
    {0x3820, 0x41},  // vertical flip off, binning on
    {0x3612, 0x59},  // analog control
    {0x3618, 0x00},  // MIPI lane count = 1

    {0x5000, 0x06},  // ISP control: BPC on, WPC on
    {0x5001, 0x01},  // ISP enable: AWB on
    {0x5002, 0x41},  // ISP control 2
    {0x5003, 0x08},  // ISP control 3
    {0x5a00, 0x08},  // test pattern off
    {0x3000, 0x00},  // IO control
    {0x3001, 0x00},
    {0x3002, 0x00},
    {0x301d, 0xf0},  // power up / on
    {0x3a18, 0x00},  // AEC ceiling
    {0x3a19, 0xf8},
    {0x3c01, 0x80},  // 50/60 Hz detection — manual
    {0x3b07, 0x0c},  // frame length
    {0x380c, 0x07},  // HTS high byte (1896)
    {0x380d, 0x68},  // HTS low byte
    {0x380e, 0x03},  // VTS high byte (984)
    {0x380f, 0xd8},  // VTS low byte

    // ── Frame size and crop for 640×480 (binning 4×) ──────────────
    // X start / end: full 2608 width → binned 640 + margins
    {0x3800, 0x00},  // x_addr_start H
    {0x3801, 0x10},  // x_addr_start L = 16
    {0x3802, 0x00},  // y_addr_start H
    {0x3803, 0x00},  // y_addr_start L = 0
    {0x3804, 0x0a},  // x_addr_end H
    {0x3805, 0x2f},  // x_addr_end L  (2607)
    {0x3806, 0x07},  // y_addr_end H
    {0x3807, 0x9f},  // y_addr_end L  (1951)

    // Output size: 640 × 480
    {0x3808, 0x02},  // x_output_size H
    {0x3809, 0x80},  // x_output_size L (640)
    {0x380a, 0x01},  // y_output_size H
    {0x380b, 0xe0},  // y_output_size L (480)

    // ── Subsample / binning ───────────────────────────────────────
    {0x3811, 0x08},  // x offset (border)
    {0x3813, 0x02},  // y offset (border)
    {0x3814, 0x31},  // x sub-sample step (binning 3+1)
    {0x3815, 0x31},  // y sub-sample step

    // ── AEC / AGC ─────────────────────────────────────────────────
    {0x3a02, 0x03},  // AEC max exposure H
    {0x3a03, 0xd8},  // AEC max exposure L
    {0x3a08, 0x01},  // B50 step H
    {0x3a09, 0x27},  // B50 step L
    {0x3a0a, 0x00},  // B60 step H
    {0x3a0b, 0xf6},  // B60 step L
    {0x3a0e, 0x03},  // max bands in 50Hz
    {0x3a0d, 0x04},  // max bands in 60Hz
    {0x3a14, 0x03},  // AEC max 50Hz exposure H
    {0x3a15, 0xd8},  // AEC max 50Hz exposure L
    {0x3a01, 0x03},  // AEC min gain

    // ── AWB / colour correction ───────────────────────────────────
    {0x3406, 0x00},  // AWB manual off (auto)
    {0x5180, 0xff},  // AWB B block
    {0x5181, 0xf2},
    {0x5182, 0x00},
    {0x5183, 0x14},
    {0x5184, 0x25},
    {0x5185, 0x24},
    {0x5186, 0x09},
    {0x5187, 0x09},
    {0x5188, 0x09},
    {0x5189, 0x88},
    {0x518a, 0x54},
    {0x518b, 0xee},
    {0x518c, 0xb2},
    {0x518d, 0x50},
    {0x518e, 0x34},
    {0x518f, 0x6b},
    {0x5190, 0x46},
    {0x5191, 0xf8},
    {0x5192, 0x04},
    {0x5193, 0x70},
    {0x5194, 0xf0},
    {0x5195, 0xf0},
    {0x5196, 0x03},
    {0x5197, 0x01},
    {0x5198, 0x04},
    {0x5199, 0x6c},
    {0x519a, 0x04},
    {0x519b, 0x00},
    {0x519c, 0x09},
    {0x519d, 0x2b},
    {0x519e, 0x38},

    // ── Lens shading correction ───────────────────────────────────
    {0x5480, 0x01},
    {0x5481, 0x08},
    {0x5482, 0x14},
    {0x5483, 0x28},
    {0x5484, 0x51},
    {0x5485, 0x65},
    {0x5486, 0x71},
    {0x5487, 0x7d},
    {0x5488, 0x87},
    {0x5489, 0x91},
    {0x548a, 0x9a},
    {0x548b, 0xaa},
    {0x548c, 0xb8},
    {0x548d, 0xcd},
    {0x548e, 0xdd},
    {0x548f, 0xea},
    {0x5490, 0x1d},

    // ── Gamma ─────────────────────────────────────────────────────
    {0x5301, 0x05},
    {0x5302, 0x0c},
    {0x5303, 0x1c},
    {0x5304, 0x2a},
    {0x5305, 0x39},
    {0x5306, 0x45},
    {0x5307, 0x53},
    {0x5309, 0x5d},
    {0x530a, 0x6b},
    {0x530b, 0x7c},
    {0x530c, 0x8c},
    {0x530d, 0xa6},
    {0x530e, 0xbc},
    {0x530f, 0xcf},
    {0x5310, 0xe0},
    {0x5311, 0xef},
    {0x5312, 0x1f},
    {0x5308, 0x25},

    // ── Sharpness / noise ─────────────────────────────────────────
    {0x5380, 0x01},
    {0x5381, 0x00},
    {0x5382, 0x00},
    {0x5383, 0x4e},
    {0x5384, 0x00},
    {0x5385, 0x0f},
    {0x5386, 0x00},
    {0x5387, 0x00},
    {0x5388, 0x01},
    {0x5389, 0x15},
    {0x538a, 0x00},
    {0x538b, 0x31},
    {0x5300, 0x08},
    {0x5303, 0x1c},
    {0x5304, 0x2a},

    // ── Stream on ─────────────────────────────────────────────────
    {0x0100, 0x01},  // streaming on

    // End sentinel
    {OV5647_REG_END, 0x00},
};

// ── I2C helpers ───────────────────────────────────────────────────────────────

static i2c_master_dev_handle_t s_dev = nullptr;

static esp_err_t sccb_write_reg(uint16_t addr, uint8_t val) {
    uint8_t buf[3] = {
        static_cast<uint8_t>(addr >> 8),
        static_cast<uint8_t>(addr & 0xFF),
        val
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100 /*ms*/);
}

static esp_err_t sccb_read_reg(uint16_t addr, uint8_t* val) {
    uint8_t addr_buf[2] = {
        static_cast<uint8_t>(addr >> 8),
        static_cast<uint8_t>(addr & 0xFF)
    };
    return i2c_master_transmit_receive(s_dev, addr_buf, 2, val, 1, 100);
}

static esp_err_t write_reg_table(const OV5647Reg* table) {
    for (const OV5647Reg* p = table; p->addr != OV5647_REG_END; ++p) {
        esp_err_t err = sccb_write_reg(p->addr, p->val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SCCB write 0x%04X failed: %s", p->addr, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t ov5647_init(i2c_master_bus_handle_t* out_bus) {
    // Power-up GPIO
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << OV5647_PWDN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ov5647_power_up(); // assert PWDN low = sensor powered

    // I2C master bus
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = static_cast<gpio_num_t>(OV5647_I2C_SDA_GPIO),
        .scl_io_num    = static_cast<gpio_num_t>(OV5647_I2C_SCL_GPIO),
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags         = { .enable_internal_pullup = true },
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // Add OV5647 device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OV5647_I2C_ADDR,
        .scl_speed_hz    = OV5647_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    vTaskDelay(pdMS_TO_TICKS(10)); // sensor power-on stabilisation

    *out_bus = bus;
    ESP_LOGI(TAG, "I2C bus init OK");
    return ESP_OK;
}

esp_err_t ov5647_check_id(i2c_master_bus_handle_t /*bus*/) {
    uint8_t id_h = 0, id_l = 0;
    ESP_ERROR_CHECK(sccb_read_reg(0x300A, &id_h));
    ESP_ERROR_CHECK(sccb_read_reg(0x300B, &id_l));
    uint16_t id = (id_h << 8) | id_l;
    if (id != 0x5647) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%04X (expected 0x5647)", id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "OV5647 detected (chip ID 0x%04X)", id);
    return ESP_OK;
}

esp_err_t ov5647_configure(i2c_master_bus_handle_t /*bus*/) {
    ESP_LOGI(TAG, "Writing 640×480 MIPI 1-lane register table…");
    vTaskDelay(pdMS_TO_TICKS(5)); // brief delay after power-on before SCCB writes
    esp_err_t err = write_reg_table(ov5647_640x480_mipi);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OV5647 configured: 640×480 RAW10 MIPI 1-lane");
    }
    return err;
}

esp_err_t ov5647_standby(i2c_master_bus_handle_t /*bus*/) {
    return sccb_write_reg(0x0100, 0x00); // software standby
}

esp_err_t ov5647_stream_on(i2c_master_bus_handle_t /*bus*/) {
    return sccb_write_reg(0x0100, 0x01); // streaming on
}

void ov5647_power_down() {
    gpio_set_level(static_cast<gpio_num_t>(OV5647_PWDN_GPIO), 1);
}

void ov5647_power_up() {
    gpio_set_level(static_cast<gpio_num_t>(OV5647_PWDN_GPIO), 0);
    vTaskDelay(pdMS_TO_TICKS(5));
}
