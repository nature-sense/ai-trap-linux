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
| Wireless | See options below | WiFi, LoRaWAN, or Meshtastic |
| Camera | TBD — CSI-compatible module | Must be compatible with Stamp-P4 CSI interface |
| Battery | LiPo, 3000–5000 mAh | Or small solar + 1000 mAh |
| Enclosure | Custom PCB + weatherproof housing | To be designed |

### Cost model (indicative)

| Item | WiFi (C6 addon) | LoRaWAN (SX1262) | Meshtastic |
|---|---|---|---|
| Stamp-P4 | £12–15 | £12–15 | £12–15 |
| Radio module | £5–8 (C6 addon) | £5–8 (SX1262 breakout) | £10–20 (T-Beam / Heltec) |
| Camera module | £5–10 | £5–10 | £5–10 |
| Battery, PCB, enclosure | £8–15 | £8–15 | £8–15 |
| **Total per trap** | **~£30–50** | **~£30–50** | **~£35–60** |
| Gateway cost | None (use existing AP) | £80–150 per site | £30–80 per mesh bridge |

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
JPEG encode → transmit (WiFi / LoRa / Meshtastic) → store event to NVS
    ↓
HP cores sleep → LP core resumes watch
```

### Power budget (estimated)

| State | Current | WiFi duration | LoRa duration |
|---|---|---|---|
| Deep sleep (LP only) | ~1–5 mA | 99% of time | 99% of time |
| Active inference | ~200–400 mA | ~100–200 ms | ~100–200 ms |
| Radio transmit | ~250–300 mA (WiFi) / ~40 mA (LoRa) | ~500 ms | ~50–500 ms |

LoRa transmit current is ~6–7× lower than WiFi and airtime is shorter, making it significantly
better for battery life in remote deployments.

A 3000 mAh LiPo at 1% duty cycle (moderate insect activity) gives weeks to months of runtime.
A small solar panel (2–5W) can sustain continuous operation.

This is fundamentally different from the CM5 which draws 4–8 W continuously regardless of activity.

---

## Network architecture

The counting-only data model produces tiny payloads (~60 bytes per event) making the trap an
ideal fit for wide-area low-power networking. Three options are viable depending on the deployment
context.

### Option A — WiFi (M5Stack Stamp-AddOn C6)

```
[Trap 001..N]  ──WiFi──►  [AP at field station]  ──4G/Ethernet──►  [Database / dashboard]
```

- Best for traps within ~100 m of a building or field station with power
- Highest power consumption of the three options
- Easiest OTA firmware updates and sampled image upload
- ESP32-C6 also supports Thread/Zigbee for short-range mesh extension
- Uses M5Stack Stamp-AddOn C6 connected to Stamp-P4 via ESP-Hosted (SPI)

### Option B — LoRaWAN (SX1262 module on SPI)

```
[Trap 001..N]  ──LoRa──►  [LoRaWAN gateway per site]  ──4G/Ethernet──►  [TTN / cloud]  ──►  [Dashboard]
```

- Best for planned deployments across a defined study area
- Range: 2–15 km line-of-sight, 1–3 km in dense woodland
- TX current ~40 mA × 50–500 ms — ~6–7× better battery life than WiFi
- Payload: 51–222 bytes per uplink — ample for count events, not for images
- **The Things Network** provides free community gateway coverage in many regions;
  a self-hosted gateway (~£100, Pi + RAK833) covers a ~5 km radius
- Replaces the C6 addon entirely — SX1262 breakout connects directly to P4 via SPI,
  simplifying the design and reducing BOM cost
- OTA firmware updates over LoRaWAN are very constrained (not recommended for routine use)

### Option C — Meshtastic (LoRa mesh, no gateway infrastructure)

```
[Trap 001..N]  ──LoRa mesh──►  [Mesh relay nodes]  ──►  [MQTT bridge node]  ──►  [Database]
```

- Best for remote deployments where installing powered gateway infrastructure is impractical
- Self-organising mesh — each node relays others' packets; coverage extends with node density
- One solar-powered relay node per ~5–10 km² can cover large study areas
- One MQTT bridge node (with internet uplink) serves the whole mesh
- Higher power than LoRaWAN due to mesh listen overhead
- Less mature for unattended sensor deployments than LoRaWAN; community firmware

### Comparison

| | WiFi (C6) | LoRaWAN (SX1262) | Meshtastic |
|---|---|---|---|
| Range | ~100 m | 2–15 km | 2–15 km/hop, mesh |
| Infrastructure | Existing AP | Gateway per site | One MQTT bridge |
| TX power | ~250–300 mA | ~40 mA | ~40 mA + listen |
| Battery impact | Poor | Excellent | Good |
| Payload limit | Unlimited | 51–222 bytes | ~240 bytes |
| Image upload | Yes | No | No |
| OTA updates | Easy | Very limited | Limited |
| Ecosystem maturity | High | High | Medium |
| Radio module | C6 addon £5–8 | SX1262 £5–8 | T-Beam/Heltec £10–20 |

### Recommendation

- **LoRaWAN** is the preferred choice for a structured large-scale deployment — lower power than
  WiFi, mature ecosystem, and The Things Network removes the need to self-host cloud infrastructure.
- **Meshtastic** is preferred where powered gateway installation is impractical (remote moorland,
  forest with no field station).
- **WiFi** is preferred where traps are near existing infrastructure and OTA updates or sampled
  image upload are required.

All three options buffer events in NVS when the uplink is unavailable and transmit when back in range.

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

3. **Radio interface** — for WiFi: P4 ↔ C6 addon protocol (ESP-Hosted SPI or ESP-AT UART?),
   latency, reliability. For LoRaWAN: SX1262 SPI driver in ESP-IDF, frequency plan and duty
   cycle compliance for deployment region. For Meshtastic: whether the firmware supports a
   headless sensor mode suitable for unattended deployment.

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
- Select radio option based on deployment context (see Network architecture above)
- Transmit detection events to a local gateway or TTN
- Test NVS buffering when gateway is unreachable
- Measure power cost of transmit cycle for chosen radio

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
| C6/LoRa radio interface complexity | Medium | Resolve in Phase 3 before committing to PCB design |
| LoRaWAN duty cycle limits (EU: 1%) | Low | Count events are infrequent; batch if needed |
| Meshtastic firmware stability for unattended sensors | Medium | Validate in Phase 3; fall back to LoRaWAN |
| Field enclosure / weatherproofing | Medium | Standard IP65 housing solutions exist |

---

## Related documents

- [esp32-p4-ai-evaluation.md](esp32-p4-ai-evaluation.md) — Full hardware and AI capability evaluation
- [luckfox-install-guide.md](luckfox-install-guide.md) — Current production platform
