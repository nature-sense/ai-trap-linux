# ESP32-P4 Low-Cost Insect Trap — Candidate Architecture

**Status:** Experimental / proposed
**Date:** March 2026
**Motivation:** Large-scale deployment of low-cost, low-power insect monitoring traps

---

## Motivation

The CM5-based trap is well suited to a small number of high-capability deployments but is too
expensive and power-hungry for large-scale monitoring networks. A trap based on the ESP32-P4
could reduce per-unit cost by 4–5× and extend battery life from days to months, making deployments
of 50–200+ traps economically viable.

---

## Target hardware

| Component | Part | Notes |
|---|---|---|
| Inference SoC | M5Stack Stamp-P4 | ESP32-P4NRW32, 32 MB PSRAM, MIPI CSI |
| Wireless | M5Stack Stamp-AddOn C6 For P4 | ESP32-C6, Wi-Fi 6, BLE 5, Thread/Zigbee |
| Camera | TBD — CSI-compatible module | Must be compatible with Stamp-P4 CSI interface |
| Battery | LiPo, 3000–5000 mAh | Or small solar + 1000 mAh |
| Enclosure | Custom PCB + weatherproof housing | To be designed |

### Cost model (indicative)

| Item | Approx. cost |
|---|---|
| Stamp-P4 | £12–15 |
| Stamp-AddOn C6 | £5–8 |
| Camera module | £5–10 |
| Battery, PCB, enclosure | £8–15 |
| **Total per trap** | **~£30–50** |

vs. ~£100–150 for a CM5-based trap. At 100 traps: ~£3,000–5,000 vs ~£10,000–15,000.

---

## AI inference

### Model: ESPDet-Pico

- Architecture: Ultralytics YOLOv11-derived, single-class or few-class detector
- Parameters: 0.36 M
- Input resolution: 224×224
- Throughput on P4: **>18 FPS** (vs ~1.6 FPS for YOLOv11n 320×320)
- Training: fine-tuned on target insect species via `espressif/esp-detection`
- Deployment: INT8 quantized via ESP-PPQ → `.espdl` format → ESP-DL

At 18+ FPS, continuous inference on a live camera stream is feasible — enabling counting of insects
passing through a trap aperture, not just triggered snapshot detection.

### Hardware acceleration

The P4 provides:
- **PIE/Xai SIMD**: 16 parallel INT8 MACs/cycle across both RISC-V cores
- **PPA**: hardware resize/crop from capture resolution to 224×224 at zero CPU cost
- **Hardware ISP**: AWB, AEC, noise reduction, demosaicing in hardware
- **Hardware H.264/JPEG encoder**: for sampled image upload

See [esp32-p4-ai-evaluation.md](esp32-p4-ai-evaluation.md) for full hardware details.

---

## Data model

For large-scale insect monitoring the scientific output is a **time-series count**, not a video
archive. This fundamentally reduces storage requirements.

### Primary data: detection events (tiny)

```json
{
  "trap_id": "trap042",
  "ts": 1743000000,
  "class": "hoverfly",
  "confidence": 0.87,
  "count": 1
}
```

Stored in NVS or transmitted immediately. A full day of busy-trap detections fits in a few KB.

### Secondary data: sampled images (occasional)

Store 1-in-N detection images to internal flash or a small SPI NOR chip. Used for:
- Model accuracy validation in field conditions
- Ground truth for retraining
- QC checks during deployment

Images are not stored for every detection in routine operation.

---

## Power architecture

### Sleep/wake cycle

```
Deep sleep (LP core active, PIR/beam watching)
    ↓  motion/beam-break interrupt
Wake HP cores (~10 ms)
    ↓
CSI capture → PPA resize → ESPDet-Pico inference (~55 ms)
    ↓  detection confirmed?
JPEG encode → transmit via C6 → store event to NVS
    ↓
HP cores sleep → LP core resumes watch
```

### Power budget (estimated)

| State | Current | Typical duration |
|---|---|---|
| Deep sleep (LP only) | ~1–5 mA | 99% of time in low-activity periods |
| Active inference | ~200–400 mA | ~100–200 ms per event |
| Wi-Fi transmit | ~200–300 mA | ~500 ms per event |

A 3000 mAh LiPo at 1% duty cycle (moderate insect activity) gives weeks to months of runtime.
A small solar panel (2–5W) can sustain continuous operation.

This is fundamentally different from the CM5 which draws 4–8 W continuously regardless of activity.

---

## Network architecture

For large-scale deployment, traps connect to a **local gateway** rather than directly to the cloud:

```
[Trap 001..N]  ──WiFi──►  [Gateway: Pi / PC / cloud VM]  ──4G/Ethernet──►  [Database / dashboard]
```

- Traps transmit small JSON events immediately on detection
- Gateway handles persistence, aggregation, and uplink
- Traps buffer events in NVS if gateway is temporarily unreachable
- Gateway can serve OTA firmware updates to the trap fleet

The C6 addon also supports **Thread/Zigbee** (IEEE 802.15.4) which enables mesh networking for
deployments where individual traps cannot reach a WiFi access point directly.

---

## Comparison with existing platforms

| | ESP32-P4 + ESPDet-Pico | Luckfox Pico Zero | CM5 |
|---|---|---|---|
| Inference speed | >18 FPS @ 224×224 | ~2–3 FPS @ 320×320 | ~20 FPS @ 320×320 |
| Active power | ~0.5–1 W | ~0.5 W | ~4–8 W |
| Sleep power | ~5–15 mW | ~50–100 mW (Linux) | ~200 mW+ (Linux) |
| Battery life (3000 mAh) | Weeks–months | Days–weeks | Hours–days |
| Wireless | C6 addon (Wi-Fi 6/BLE/Thread) | AIC8800DC built-in | Built-in |
| OS | FreeRTOS (ESP-IDF) | Linux (busybox) | Linux (Debian) |
| Storage | NVS + flash (no OS) | SD/eMMC | eMMC |
| Per-unit cost | ~£30–50 | ~£20–30 | ~£100–150 |
| Codebase reuse | None — full rewrite | Partial | Full |

---

## Key unknowns to resolve experimentally

These must be validated before committing to this architecture:

1. **ESPDet-Pico accuracy on target species** — fine-tuned single-class accuracy in field conditions
   (variable lighting, partial occlusion, dead insects, debris). This is the highest-risk unknown.

2. **Camera module compatibility** — which CSI camera module works with the Stamp-P4, and whether
   M5Stack provides a ready-made option or a custom module is required.

3. **P4 ↔ C6 communication** — M5Stack's implementation of the Stamp-AddOn interface: protocol
   (ESP-Hosted SPI or ESP-AT UART?), latency, reliability, and whether standard ESP-Hosted drivers
   work out of the box.

4. **Real-world power consumption** — measured wake-detect-transmit-sleep cycle current vs. estimates
   above. LP core sleep current with PIR interrupt wakeup.

5. **Field enclosure** — weatherproofing, condensation management, lens cleaning in outdoor deployment.

---

## Recommended experimental sequence

Do not attempt the full pipeline in one step. Validate the highest-risk elements first:

### Phase 1 — Inference accuracy (bench, no camera needed)
- Set up ESP-IDF + ESP-DL on Stamp-P4
- Load pre-built ESPDet-Pico model
- Run inference on a set of labelled insect test images loaded from flash
- Measure accuracy, latency, and memory usage
- **Go/no-go: is accuracy acceptable for target species?**

### Phase 2 — Camera pipeline (bench)
- Attach a CSI camera module
- Capture frames, run PPA resize to 224×224
- Feed live frames to ESPDet-Pico
- Verify DMA buffer management (internal SRAM constraint)
- Measure end-to-end latency and power

### Phase 3 — Wireless (bench)
- Add Stamp-AddOn C6
- Transmit detection events to a local gateway
- Test NVS buffering when gateway is unreachable
- Measure power cost of transmit cycle

### Phase 4 — Power profiling (bench)
- Implement full sleep/wake cycle with PIR interrupt
- Measure current at each state
- Validate battery life estimates

### Phase 5 — Field trial
- Deploy 2–3 units alongside an existing CM5 trap for ground truth comparison
- Run for 2–4 weeks
- Compare detection counts and sampled images against CM5 ground truth

---

## Known risks

| Risk | Severity | Mitigation |
|---|---|---|
| ESPDet-Pico accuracy insufficient for target species | High | Phase 1 validation; custom training dataset |
| P4 silicon revision supply chain (v1.0 vs v1.3) | Medium | Check revision before bulk order; see [esp32-p4-ai-evaluation.md](esp32-p4-ai-evaluation.md) |
| ESP-DL model format instability across versions | Low | Pin ESP-IDF + ESP-DL versions in firmware |
| DMA SRAM constraint for camera buffers | Medium | Profile in Phase 2; reduce resolution if needed |
| C6 addon interface complexity | Medium | Resolve in Phase 3 before committing to PCB design |
| Field enclosure / weatherproofing | Medium | Standard IP65 housing solutions exist |

---

## Related documents

- [esp32-p4-ai-evaluation.md](esp32-p4-ai-evaluation.md) — Full hardware and AI capability evaluation
- [luckfox-install-guide.md](luckfox-install-guide.md) — Current production platform
