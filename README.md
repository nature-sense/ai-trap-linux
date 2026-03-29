# ai-trap-linux

Linux-based firmware for ai-trap insect monitoring stations.
Targets the Raspberry Pi 5 and Luckfox Pico Zero; designed to accommodate
future Linux-capable trap hardware in the same repository.

## Related repositories

| Repository | Description |
|---|---|
| [ai-trap-models](https://github.com/nature-sense/ai-trap-models) | Model source (`.pt`) and conversion pipeline — submoduled at `firmware/models/` |
| [ai-trap-api](https://github.com/nature-sense/ai-trap-api) | OpenAPI spec and Flutter client codegen — submoduled at `api/` |
| [ai-trap-esp](https://github.com/nature-sense/ai-trap-esp) | ESP32-based trap variants |

---

## Hardware targets

| Target | Board | SoC | Camera | Inference |
|---|---|---|---|---|
| `libcamera` | Raspberry Pi 5 / CM5 | BCM2712 Cortex-A76 | IMX708 via libcamera | NCNN (CPU) |
| `v4l2` | Luckfox Pico Zero | RV1106 Cortex-A7 | IMX415 via V4L2 | NCNN (CPU) or RKNN NPU |

---

## Architecture

Each target runs a single binary that owns the full pipeline:

```
Camera capture  →  YOLO11n inference  →  Decoder  →  ByteTracker
                                                           │
                                             ┌─────────────┼─────────────┐
                                        SQLite3        JPEG crops     HTTP server
                                        (detections)   (disk)         ├─ REST API   :8080
                                                                       ├─ MJPEG stream :9000
                                                                       └─ SSE events  :8081
```

Shared code (`firmware/src/`) compiles into both targets. Platform-specific capture
and inference backends are selected at build time.

### Source layout

```
firmware/
├── main_libcamera.cpp          Pi 5 entry point
├── main_v4l2.cpp               Luckfox entry point
├── models/                     ai-trap-models submodule (ncnn weights, rknn model)
└── src/
    ├── common/
    │   ├── wifi_manager        AP / station switching (NetworkManager on Pi, hostapd on Luckfox)
    │   ├── ble_gatt_server     BLE GATT provisioning
    │   ├── epaper_display      Waveshare 2.13" e-Paper HAT (SPI)
    │   └── config_loader       TOML config parsing
    ├── io/
    │   ├── libcamera_capture   IMX708 capture (Pi 5)
    │   ├── v4l2_capture        IMX415 capture (Luckfox)
    │   └── rknn_infer          RKNN NPU inference (Luckfox, optional)
    ├── pipeline/
    │   ├── decoder             YOLO11n output decoding (anchor-grid and end-to-end)
    │   ├── tracker             ByteTracker multi-object tracking
    │   ├── persistence         SQLite3 detection logging
    │   ├── crop_saver          Async JPEG crop writing
    │   └── exif_writer         EXIF geo-tagging
    └── server/
        ├── http_server         REST API
        ├── mjpeg_streamer      Live MJPEG stream
        └── sse_server          Server-sent events
```

---

## Building

### Prerequisites

| Tool | Version |
|---|---|
| Meson | ≥ 1.3 |
| Ninja | any recent |
| C++17 compiler | GCC or Clang |

The NCNN inference library is a submodule under `subprojects/ncnn/` and is
built from source as part of the Meson wrap system — no separate installation needed.

### Clone

```bash
git clone --recurse-submodules https://github.com/nature-sense/ai-trap-linux.git
cd ai-trap-linux
```

### Pi 5 — native build on the board

```bash
meson setup buildDir -Dtarget=libcamera
ninja -C buildDir
```

### Pi 5 — cross-compile from Linux x86-64

Install the toolchain:
```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

Set up a sysroot with libcamera and SQLite3 headers for aarch64, then:
```bash
meson setup buildDir \
    --cross-file cross/pi5-aarch64.ini \
    -Dtarget=libcamera \
    -Dpi5_sysroot=/path/to/pi5-sysroot
ninja -C buildDir
```

### Luckfox Pico Zero — cross-compile

Build the cross-compiled dependencies first (requires the Luckfox SDK toolchain):
```bash
bash scripts/build-luckfox-deps.sh
```

Then build:
```bash
meson setup buildDir \
    --cross-file cross/luckfox-rv1106.ini \
    -Dtarget=v4l2
ninja -C buildDir
```

#### RKNN NPU (optional, ~10× faster inference on Luckfox)

Copy `librknnrt.so` and `rknn_api.h` from the Luckfox SDK into the sysroot
(see `cross/luckfox-rv1106.ini` for expected paths), then:

```bash
meson setup buildDir \
    --cross-file cross/luckfox-rv1106.ini \
    -Dtarget=v4l2 \
    -Duse_rknn=true
ninja -C buildDir
```

The RKNN model (`firmware/models/yolo11n-320/model.rknn`) is generated automatically
when `model.pt` is pushed to [ai-trap-models](https://github.com/nature-sense/ai-trap-models).

---

## Deployment

### Raspberry Pi 5 / CM5

```bash
# Build package and check it
bash scripts/make-package.sh

# Deploy to a trap
scp -r package/ trap@trap006.local:~/
ssh trap@trap006.local 'sudo bash ~/package/install.sh && sudo reboot'
```

### Luckfox Pico Zero

See [`docs/luckfox-install-guide.md`](docs/luckfox-install-guide.md) for the full procedure.

---

## Configuration

Runtime behaviour is controlled by `trap_config.toml`. Key sections:

| Section | Controls |
|---|---|
| `[trap]` | Device ID, GPS coordinates for EXIF geo-tagging |
| `[model]` | NCNN/RKNN model paths, input size, class count |
| `[detection]` | Confidence threshold, NMS, minimum box size |
| `[tracker]` | ByteTracker thresholds, min hits, max missed frames |
| `[camera]` | Capture resolution, framerate, ISP tuning |
| `[stream]` | MJPEG port and resolution |
| `[api]` | REST API port |
| `[wifi]` | AP/station mode, SSID, credentials path |

---

## CI

| Workflow | Trigger | Artefact |
|---|---|---|
| Build · Pi 5 | push / PR | `yolo_libcamera` aarch64 binary |
| Build · Luckfox Pico Zero | push / PR | `yolo_v4l2` armv7 binary |

Model conversion (`.pt` → ncnn / ONNX / RKNN) runs in the
[ai-trap-models](https://github.com/nature-sense/ai-trap-models) repository.

---

## Known hardware issues

See [`CLAUDE.md`](CLAUDE.md) for confirmed hardware issues and fixes including:
- CM5 reboot loop caused by static `dtoverlay=imx708` in `config.txt`
- Kernel panic at `capture_width=2304` on RV1106 (use `1536×864`)
- libcamera IPA kernel panic with bundled IPA modules
