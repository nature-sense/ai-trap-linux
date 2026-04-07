# ESP32-P4 Model Workflow — ESPDet-Pico Creation and Deployment

**Status:** In progress
**Date:** April 2026
**Scope:** How to train, quantize, convert, and flash the INT8 object detection model for the ESP32-P4 insect trap firmware

---

## Overview

"ESPDet-Pico" is the project name for the INT8 detection model that runs on the ESP32-P4 via ESP-DL.
It does not refer to a specific pre-existing model — it is the name given to whatever architecture
is trained, quantized, and converted for this deployment. The model is stored in the `model` flash
partition (4 MB at `0x290000`) and loaded into PSRAM at runtime by `InferenceTask`.

---

## Inference Pipeline Summary

The full pipeline from sensor to detection event:

```
OV5647 (RAW10)
    │  MIPI CSI-2, 1-lane, 200 Mbps
    ▼
ESP32-P4 CSI controller + ISP
    │  Demosaic / AWB / AEC → YUV420 NV12 224×224
    ▼
DMA buffer pool (2 × 75 KB internal SRAM)
    │  g_frame_queue
    ▼
PPA hardware accelerator
    │  YUV420 → RGB888 224×224 (zero CPU cost)
    ▼
s_rgb_buf (150 KB static internal SRAM)
    │  dl::TensorBase wrapper [1, 224, 224, 3] UINT8
    ▼
ESP-DL — ESPDet-Pico INT8
    │  ~55 ms on both HP cores
    ▼
Postprocessing (conf threshold filter)
    │  g_detection_queue
    ▼
RadioTask — HTTP POST / NVS ring buffer
```

---

## Step 1 — Choose an Architecture

The model must fit in the 4 MB `model` partition and run in under ~100 ms on the ESP32-P4.
The target spec is ~224×224 INT8, ~0.3–0.5 M parameters.

| Architecture | Params | INT8 size | Notes |
|---|---|---|---|
| **NanoDet-Pico** | ~0.3 M | ~400–600 KB | Best fit — designed for mobile edge |
| YOLOv8n | ~3.2 M | ~3.2 MB | Borderline; tight in 4 MB |
| YOLOv5n6 | ~1.8 M | ~1.8 MB | Acceptable |
| YOLOv10n | ~2.3 M | ~2.3 MB | Acceptable |
| Custom FCOS-tiny | variable | — | Anchor-free, easy to prune |

**Recommended starting point:** `nanodet-pico-224` — its native input resolution is 224×224,
parameter count is ~0.3 M, and it is explicitly designed for constrained inference targets.

---

## Step 2 — Dataset

Bounding-box annotated images of the insect classes to be detected.

### Existing datasets

| Dataset | Classes | Images | Format | Notes |
|---|---|---|---|---|
| **IP102** | 102 insect pests | ~75 k | PASCAL VOC | Good general starting point |
| **iNaturalist** | thousands | millions | COCO-style | Needs filtering and cropping |
| **DeepInsect** | ~60 | ~23 k | COCO | Lab-quality images |

### Custom capture

Images captured directly from the trap camera give the best domain match. Use
[Label Studio](https://labelstud.io) or [CVAT](https://cvat.ai) to annotate.

Minimum recommendation: **500 images per class** for fine-tuning from pretrained weights;
**2 000+ per class** for training from scratch.

---

## Step 3 — Training

### NanoDet-Pico

```bash
git clone https://github.com/RangiLyu/nanodet
cd nanodet
pip install -r requirements.txt

# Edit config for 224×224 and your class count:
#   config/legacy_v0.x_configs/nanodet-pico.yml
#     input_size: [224, 224]
#     num_classes: <N>
#     data.train_dataset.img_path: /path/to/train
#     data.val_dataset.img_path:   /path/to/val

python tools/train.py config/nanodet-pico.yml
```

### YOLOv8 (simpler tooling, larger model)

```bash
pip install ultralytics

# insects.yaml: path, train, val, nc, names
yolo detect train \
    data=insects.yaml \
    model=yolov8n.pt \
    imgsz=224 \
    epochs=100 \
    batch=32
```

**Transfer learning tip:** Always start from COCO-pretrained weights and fine-tune on your
insect dataset. Training from scratch requires significantly more data and time.

---

## Step 4 — Export to ONNX

ESP-DL requires ONNX input. Key requirements:

- Opset **11 or 12** (not higher)
- **Static input shapes** — no dynamic axes
- Input shape: `[1, 3, 224, 224]` (NCHW) or `[1, 224, 224, 3]` (NHWC)
- NMS included inside the model is preferred (simplifies postprocessing)

### NanoDet

```bash
python tools/export_onnx.py \
    --cfg_path  config/nanodet-pico.yml \
    --model_path checkpoints/model_best.ckpt \
    --out_path  espdet_pico_224.onnx
```

### YOLOv8

```bash
yolo export \
    model=runs/detect/train/weights/best.pt \
    format=onnx \
    imgsz=224 \
    opset=12 \
    dynamic=False
```

Verify the exported model:

```bash
python -c "
import onnx
m = onnx.load('espdet_pico_224.onnx')
onnx.checker.check_model(m)
print('Input:', [i.name for i in m.graph.input])
print('Output:', [o.name for o in m.graph.output])
"
```

---

## Step 5 — INT8 Quantization and Conversion to `.espdl`

ESP-DL v3.x uses [PPQ](https://github.com/openppl-public/ppq) internally for quantization,
then serialises to FlatBuffers `.espdl` format.

### Install the ESP-DL Python tools

```bash
pip install esp-dl
# Requires: torch, onnx, onnxruntime, ppq
```

Or install from source for the latest v3.x tooling:

```bash
git clone https://github.com/espressif/esp-dl
cd esp-dl
pip install -e ".[tools]"
```

### Prepare calibration data

100–500 representative images from the actual deployment environment (same lighting, angle,
resolution as the trap camera). These are used only for INT8 scale calibration — they are
never embedded in the model file.

```python
# prepare_calib.py
import numpy as np
from PIL import Image
import glob

calib_images = []
for path in sorted(glob.glob("calib/*.jpg"))[:200]:
    img = Image.open(path).resize((224, 224)).convert("RGB")
    arr = np.array(img, dtype=np.float32) / 255.0   # [224, 224, 3]
    calib_images.append(arr[np.newaxis])             # [1, 224, 224, 3]

print(f"Calibration set: {len(calib_images)} images")
```

### Convert

```python
from esp_dl.tools.convert_tool import convert

convert(
    onnx_model_path  = "espdet_pico_224.onnx",
    espdl_model_path = "espdet_pico_224_int8.espdl",
    calibration_data = calib_images,    # list of np arrays [1, 224, 224, 3]
    target_chip      = "esp32p4",
    quantization     = "int8",
    input_shape      = [1, 224, 224, 3],
)
```

> **Note:** The exact API may vary between ESP-DL versions. Check
> `esp-dl/tools/convert_tool.py` for the current interface.

---

## Step 6 — Verify the Output

```bash
ls -lh espdet_pico_224_int8.espdl
# NanoDet-Pico INT8: typically 400–600 KB — well within 4 MB partition
```

Check basic FlatBuffers validity:

```bash
python -c "
with open('espdet_pico_224_int8.espdl', 'rb') as f:
    data = f.read()
print(f'Model file: {len(data)/1024:.1f} KB')
assert len(data) < 4 * 1024 * 1024, 'Model exceeds 4 MB partition!'
print('Size check passed.')
"
```

---

## Step 7 — Flash to the `model` Partition

The `model` partition starts at `0x290000` (see `partitions.csv`).

### Flash model only (without reflashing the app)

```bash
esptool.py --chip esp32p4 \
    -p /dev/cu.usbserial-* \
    -b 460800 \
    write_flash 0x290000 espdet_pico_224_int8.espdl
```

### Flash model using parttool (by partition name)

```bash
parttool.py --port /dev/cu.usbserial-* \
    write_partition \
    --partition-name model \
    --input espdet_pico_224_int8.espdl
```

### Flash everything together

```bash
# From the firmware-esp32p4 build directory:
idf.py flash                        # flashes bootloader + app + partition table

# Then separately flash the model:
esptool.py ... write_flash 0x290000 espdet_pico_224_int8.espdl
```

---

## Step 8 — Output Tensor Format Compatibility

The postprocessing in `inference_task.cpp` (`postprocess_and_emit()`) expects:

```
Output tensor shape: [1, N, 6]
Row layout: [x1, y1, x2, y2, confidence, class_id]  — values normalised [0, 1]
```

This is the standard post-NMS output format from ESP-DL's detector heads.

If your model has a different output layout (e.g., raw NanoDet output is
`[1, N, 4 + num_classes]` before argmax), you have two options:

**Option A — Fix in ONNX** (preferred): Add a reshape + argmax + concat node to the ONNX
graph before conversion, so the `.espdl` model already emits `[1, N, 6]`.

**Option B — Fix in firmware**: Adjust `postprocess_and_emit()` in `inference_task.cpp` to
match the model's actual output format. This keeps the model conversion simple but requires
a firmware rebuild.

---

## Fastest Path to a Working System

If you want to validate the full firmware pipeline before a domain-specific model is trained:

1. **Use a model from the ESP-DL model zoo**

   ```bash
   # github.com/espressif/esp-dl — models/ directory
   # Contains pre-converted COCO detectors in .espdl format
   # Flash one of these to confirm the firmware pipeline works end-to-end
   ```

2. **Fine-tune from COCO weights on your insect images**

   Start from a COCO-pretrained NanoDet or YOLOv8n checkpoint and fine-tune on
   your insect dataset for 20–50 epochs. Much faster than training from scratch
   and gives strong results even with modest dataset sizes (~500 images/class).

---

## Partition Layout Reference

```
Offset      Size    Label     Description
─────────────────────────────────────────────────────
0x9000      24 KB   nvs       Non-volatile storage
0xF000       4 KB   phy_init  WiFi PHY calibration data
0x10000    2.5 MB   factory   Application binary
0x290000     4 MB   model     ESPDet-Pico .espdl model  ← write here
0x690000   1.5 MB   storage   SPIFFS (logs, samples)
```

Total flash: 16 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`)
