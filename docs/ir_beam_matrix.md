# IR Beam Matrix for Insect Detection

## 1. Concept

A single IR beam produces one time-series signal encoding wingbeat frequency and harmonics.
A 1D array of narrow beams across a flight path produces a **2D image: position × time**
that captures the full flight morphology of the insect as it passes through.

```
beam
position
    16 │░░░░░░░█████░░░░░░░░░░░████████░░░░░
    15 │░░░░░██████████░░░░░░███████████░░░
    14 │░░░████████████████████████████████  ← body
    13 │░░░████████████████████████████████  ← body
    12 │░░░░███████████░░░░░░████████████░░
    11 │░░░░░░░░████████░░░░░░░░███████░░░░
    10 │░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
     └─────────────────────────────────────  time →
```

The central beams (body) remain interrupted continuously. The outer beams (wings) flicker
at wingbeat frequency. The spatial extent changes as the wings sweep up and down. The
complete pattern is a **fingerprint of the insect's flight morphology**.

---

## 2. What a Beam Matrix Adds

| Feature | Single Beam | Beam Matrix |
|---|---|---|
| Wingbeat frequency | Yes | Yes |
| Harmonic structure | Yes | Yes per beam |
| Body width | No | Yes from central beam count |
| Wing span | No | Yes from total interruption width |
| Wing stroke amplitude | No | Yes from envelope of outer beam modulation |
| Wing shape | No | Yes from spatial profile over time |
| Flight speed | No | Yes from sweep rate if beam spacing known |
| Flight direction | No | Yes |
| Body length | No | Yes if 2D matrix used |

Wing span alone is highly discriminative — a mosquito, a bee, and a hawk-moth have very
different spans. Combined with wingbeat frequency, the identification power increases
dramatically.

---

## 3. 1D Array vs 2D Matrix

### 1D Array (line of beams across funnel entrance) — Recommended

- Simple to build and read out
- Produces the position × time image shown above
- 8–16 beams across a 20 mm funnel = 1.25–2.5 mm resolution
- Sampling at 5–10 kHz captures wingbeat structure cleanly

### 2D Matrix (grid of beams)

- Gives a full 2D cross-section at each moment
- Wing shape in 2D — much richer information
- Cross-talk between beams is a serious problem without careful optical collimation
- Read-out complexity increases as N × M
- Probably unnecessary where flight direction is constrained by a funnel

---

## 4. Collimation — Making the Beams Narrow

For beams to be genuinely narrow and not illuminate adjacent photodiodes, collimation
is required:

| Method | Cost | Effectiveness |
|---|---|---|
| Narrow-angle IR LEDs (5–10°) + aperture on detector | Very low | Good enough for 2–3 mm spacing |
| Small lens per LED/detector pair | Low | Good |
| Optical fibre guides | Moderate | Excellent — sub-mm beams |
| Laser diodes | Moderate | Excellent — eye safety consideration applies |

For a 1D array at funnel scale, narrow-angle LEDs with a small aperture (a drilled plate)
in front of the photodiodes is sufficient and very cheap.

---

## 5. Read-out Options

At 16 beams × 10 kHz, 160,000 samples per second must be read. Options:

| Method | Complexity | Notes |
|---|---|---|
| Sequential LED activation | Simple | Effective rate per beam = total rate / N beams |
| Parallel + analog multiplexer (e.g. CD74HC4051) | Low | All LEDs on; switch photodiodes rapidly |
| I2C / SPI ADC array (e.g. ADS1115) | Easy software | Slower |
| SPI ADC (e.g. MCP3208, 8-ch, 100 ksps) | Low | Handles 16-beam array comfortably on Luckfox |

The Luckfox GPIO with a fast SPI ADC is the natural fit for this project.

---

## 6. The Data as a CNN Input

A 100 ms window at 10 kHz across 16 beams produces a **16 × 1000 pixel image**. This is
small by deep learning standards. A compact 2D CNN (MobileNet-style or smaller) trained
on these images could classify:

- Order / family level from wing span + wingbeat frequency combination
- Individual species where wing morphology is sufficiently distinct

This fits comfortably on the Luckfox RV1103 NPU in RKNN format. The input is tiny and
the model can be very compact — it would run as a companion to the visual crop classifier
triggered by the IR crossing event rather than a camera frame.

---

## 7. Integration with the AI Trap Pipeline

For the funnel entrance of the AI trap, the 1D IR beam matrix slots naturally into the
existing pipeline:

```
Insect approaches funnel
        │
        ▼
IR beam matrix (1D array, 16 beams)
→ 16 × N time-series image
→ CNN on Luckfox NPU (RKNN)
→ wing span + WBF classification
        │
        ▼
Insect enters camera field of view
→ YOLOv11n visual detection (NPU)
→ ByteTracker confirmation
→ crop saved
        │
        ▼
Fusion: IR classification + visual classification
→ high-confidence identification stored in SQLite
```

The IR crossing event also provides a precise entry timestamp and individual count —
useful independently of whether the visual detection succeeds, and resilient to lighting
conditions that may degrade the camera channel.

---

## 8. Summary

A 1D IR beam matrix at the trap funnel entrance is a low-cost, high-value addition to
the existing visual detection pipeline. With 8–16 narrow beams, a fast SPI ADC, and a
compact CNN running on the Luckfox NPU, it adds wing span, body width, stroke amplitude,
and flight direction to the feature set — information that a single beam and a camera
alone cannot provide. Fused with the visual classification, it significantly raises
identification confidence, particularly for species that are visually similar but differ
in wing morphology or wingbeat frequency.
