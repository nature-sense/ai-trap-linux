# ai-trap ESP32-P4 Firmware

Insect counting trap firmware for the **ESP32-P4NRW32** SoC.

Counts insects passing through the trap aperture in real time.
Transmits detection events as compact JSON over WiFi (or LoRaWAN — Phase 3).
No image storage in routine operation — count events only (~60 bytes each).

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| SoC | M5Stack Stamp-P4 (ESP32-P4NRW32) | Dual HP RISC-V @ 400 MHz, 32 MB PSRAM |
| Camera | OV5647 MIPI CSI-2 module | 1-lane MIPI, 640×480 capture → 224×224 ISP output |
| PIR | HC-SR501 or equivalent | GPIO5 (configurable in NVS) |
| Radio (WiFi) | M5Stack Stamp-AddOn C6 | ESP-Hosted SPI to Stamp-P4 |
| Radio (LoRa) | SX1262 breakout | Phase 3 — not yet implemented |

### GPIO pinout (defaults)

| Signal | GPIO | Notes |
|---|---|---|
| OV5647 PWDN | 6 | Active-high power-down |
| OV5647 SCL (SCCB) | 8 | I2C master |
| OV5647 SDA (SCCB) | 9 | I2C master |
| OV5647 MIPI D0+/- | CSI lane 0 | Differential — PCB routing |
| OV5647 MIPI CLK+/- | CSI clock | Differential — PCB routing |
| PIR OUT | 5 | Active-high, feeds LP core GPIO interrupt |

---

## Prerequisites

- ESP-IDF **≥ 5.3.0** (required for P4 CSI controller and LP core APIs)
- Python 3.10+, CMake ≥ 3.16
- Espressif managed components CLI (`idf.py add-dependency`)

```bash
# Install ESP-IDF (if not already)
git clone --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf
cd ~/esp/esp-idf && git checkout v5.3 && ./install.sh esp32p4
source ~/esp/esp-idf/export.sh
```

---

## Build

```bash
cd firmware-esp32p4

# Set target (once)
idf.py set-target esp32p4

# Fetch managed components (first build only — needs internet)
idf.py update-dependencies

# Build
idf.py build
```

---

## Flash model

The ESPDet-Pico `.espdl` model must be flashed to the `model` partition
**separately** from the app:

```bash
# Download or convert ESPDet-Pico INT8 model:
#   https://github.com/espressif/esp-dl → models/espdet_pico_224_int8.espdl
#
# Flash to model partition (offset 0x290000 per partitions.csv):
esptool.py --port /dev/ttyUSB0 --baud 921600 \
    write_flash 0x290000 espdet_pico_224_int8.espdl
```

---

## Flash firmware

```bash
idf.py -p /dev/ttyUSB0 -b 921600 flash
idf.py -p /dev/ttyUSB0 monitor
```

---

## Configure (NVS provisioning)

WiFi credentials and trap ID are stored in NVS. Set them via the serial monitor
REPL or by flashing an NVS partition image.

### Via Python helper (recommended)

```bash
# Install nvs_partition_gen tool (comes with ESP-IDF)
# Create config CSV:
cat > nvs_config.csv << 'EOF'
key,type,encoding,value
trap_cfg,namespace,,
trap_id,data,u16,1
wifi_ssid,data,string,MyNetwork
wifi_pass,data,string,MyPassword
server_url,data,string,http://192.168.1.100:8080/api/detections
idle_timeout,data,u32,30
pir_gpio,data,u8,5
conf_thresh,data,u32,1045220557
EOF

# Generate NVS partition image
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate nvs_config.csv nvs.bin 0x6000

# Flash NVS partition
esptool.py --port /dev/ttyUSB0 write_flash 0x9000 nvs.bin
```

> Note: `conf_thresh` is stored as `uint32` bitcast from `float 0.45`.
> `0.45f` as hex IEEE-754 = `0x3E666666` = 1045220198 decimal.

---

## Server API

Detection events are POSTed to `server_url` as JSON:

```json
{"trap":1,"ts":1743000042,"cls":0,"conf":87,"cnt":1}
```

| Field | Type | Description |
|---|---|---|
| `trap` | uint | Trap ID |
| `ts` | int | Unix timestamp (seconds) |
| `cls` | uint | Species class index (0 = insect, single-class model) |
| `conf` | uint | Confidence 0–100 |
| `cnt` | uint | Detection count (always 1 for now; future: burst coalescing) |

A simple receiving server (Python):

```python
from flask import Flask, request
app = Flask(__name__)

@app.route('/api/detections', methods=['POST'])
def detection():
    print(request.json)
    return '', 204

app.run(host='0.0.0.0', port=8080)
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│ DEEP SLEEP  (HP cores off, LP core monitors PIR)  ~1–5 mA        │
└──────────────────────┬───────────────────────────────────────────┘
                       │ PIR POSEDGE → ulp_lp_core_wakeup_main_processor()
┌──────────────────────▼───────────────────────────────────────────┐
│ BOOT (~10 ms)  NVS init → PM config → tasks start                │
└──────────────────────┬───────────────────────────────────────────┘
                       │
        ┌──────────────┼──────────────┐
        │              │              │
 ┌──────▼──────┐ ┌─────▼──────┐ ┌────▼──────┐
 │ CameraTask  │ │ Inference  │ │ RadioTask │
 │  Core 0, 6  │ │  Core 1, 5 │ │  Core 0,3 │
 │             │ │            │ │           │
 │ OV5647 CSI  │ │ PPA resize │ │ WiFi POST │
 │ 640×480 RAW │ │ YUV→RGB888 │ │ NVS buf   │
 │ ISP→224×224 │ │ ESPDet-Pico│ │ retry     │
 │ YUV420      │ │ MULTI_CORE │ │           │
 └──────┬──────┘ └─────┬──────┘ └─────▲─────┘
        │ frame_queue  │ detection_q   │
        └──────────────┘───────────────┘
                       │ idle for N s
               ┌───────▼────────┐
               │  PowerTask     │
               │  Core 0, pri 1 │
               │  drain queues  │
               │  load LP ULP   │
               │  deep_sleep    │
               └────────────────┘
```

### Task inventory

| Task | Core | Priority | Stack | Role |
|---|---|---|---|---|
| CameraTask | 0 | 6 | 6 KB | OV5647 init + CSI frame pool |
| InferenceTask | 1 | 5 | 12 KB | PPA + ESPDet-Pico + emit events |
| RadioTask | 0 | 3 | 8 KB | WiFi STA + HTTP POST + NVS ring buffer |
| PowerTask | 0 | 1 | 4 KB | Idle timer + orderly deep sleep |

### Queue inventory

| Queue | Depth | Item | Producer | Consumer | Full policy |
|---|---|---|---|---|---|
| `g_frame_queue` | 2 | `FrameBuffer*` | Camera ISR | InferenceTask | Drop frame |
| `g_buffer_pool_queue` | 2 | `FrameBuffer*` | InferenceTask | CameraTask | Never full |
| `g_detection_queue` | 16 | `DetectionEvent` | InferenceTask | RadioTask | Drop oldest |

### Memory layout

```
Internal SRAM (768 KB)
  FreeRTOS kernel + overhead   ~80 KB
  Task stacks (4 tasks)        ~30 KB
  Static TCBs + queue storage   ~8 KB
  Frame buffer A (224×224 YUV420) ~75 KB
  Frame buffer B (224×224 YUV420) ~75 KB
  inference_buf (224×224 RGB888) ~150 KB
  ESP-DL hot activations        ~100 KB
  Headroom / drivers            ~250 KB

PSRAM (32 MB)
  ESPDet-Pico weights            ~2 MB
  ESP-DL work buffers            ~2 MB
  Available                     ~28 MB
```

---

## Power states (estimated)

| State | Current | Duration |
|---|---|---|
| Deep sleep (LP core + PIR) | ~1–5 mA | ~99% of time (low activity) |
| Boot + pipeline init | ~150–200 mA | ~50 ms (one-time per wakeup) |
| Camera + inference running | ~200–400 mA | ~55 ms per frame |
| WiFi TX | ~250–300 mA | ~500 ms per event |

On a 3000 mAh LiPo at 1% duty cycle: **weeks to months** of runtime.

---

## Known limitations / next phases

| Item | Phase | Status |
|---|---|---|
| LoRaWAN (SX1262) RadioTask path | 3 | Stub only — WiFi implemented |
| RTC / NTP wall clock (accurate timestamps) | 2 | Uses `esp_timer_get_time()` (since boot) |
| Image sampling (1-in-N JPEG for QC) | 2 | Not implemented |
| OTA firmware update | 3 | USB reflash only |
| Multi-species model | Future | ESPDet-Pico is single-class |
| LoRa duty-cycle compliance (EU 1%) | 3 | Count events are infrequent |

---

## Related documents

- `docs/esp32-p4-insect-trap-architecture.md` — System-level design, network options, experimental roadmap
- `docs/esp32-p4-runtime-architecture.md` — FreeRTOS task design, queue layout, memory map
- `docs/esp32-p4-ai-evaluation.md` — ESP32-P4 AI capability benchmarks
