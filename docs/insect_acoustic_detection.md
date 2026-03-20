# Insect Detection Using Sound and IR Wingbeat Sensing

## 1. The Core Challenge

Wing sounds are quiet. Unlike bird calls or frog chorus, insect wingbeats are typically
200–600 Hz at very low amplitude — easily swamped by wind, vegetation, traffic, or other
insects. This fundamentally limits pure acoustic approaches in open field conditions.

---

## 2. State of the Art by Approach

### 2.1 Optical Wingbeat Sensing (Best Practical Results)

Not strictly acoustic, but the dominant method for wingbeat frequency detection in field
instruments. An IR LED + photodiode pair across a flight path interrupts as the insect
flies through — the modulation of the beam encodes wingbeat frequency directly, with no
ambient noise problem. The **KInsecta** system (Raspberry Pi based, multimodal) uses this
approach.

**Lund University's Scheimpflug lidar** (Mikkel Brydegaard's group) takes this further — a
continuous-wave lidar measures optical backscatter from insects in flight, extracting
wingbeat frequency, body length, wing loading, and wingbeat harmonics at ranges up to
several hundred metres. It is the most information-rich method but expensive and not yet
field-deployable at low cost.

### 2.2 Acoustic CNN on Spectrograms

The standard ML approach: record with a MEMS microphone, compute a mel spectrogram,
classify with a CNN. Works well in:

- Controlled flight tunnels
- Near-field (insect within ~10 cm of microphone)
- Quiet environments

**Mosquitoes** are the most studied group here, driven by malaria research funding. The
**HumBugDB** dataset (Google/Oxford, Kiskin et al.) is the largest labelled wingbeat audio
corpus. Best reported accuracy in laboratory conditions is >95%; in field conditions it
drops to 60–75% due to noise and range.

For other insect orders the datasets are sparse and accuracy figures are much lower.

### 2.3 Stridulation Detection

Crickets, grasshoppers, and some beetles produce loud, structured stridulation sounds that
are far easier to detect acoustically than wingbeats. Species-level identification of
crickets and grasshoppers from sound is achievable reliably in field conditions. Less
relevant for moths, flies, or beetles in flight.

### 2.4 Foundation Models for Bioacoustics

**BirdNET** (Cornell Lab) is the gold standard for bird sound identification — a deep CNN
trained on millions of labelled recordings achieving near-expert accuracy across thousands
of species. An insect equivalent does not yet exist at the same scale. Attempts include
**InsectSet32 / InsectSet66** (small labelled datasets) and the DCASE 2022/2023 bioacoustics
challenge. The gap vs birds is primarily data: there are vastly more labelled bird
recordings than insect wing sounds.

### 2.5 Summary Table

| Target | Method | Field Accuracy | Maturity |
|---|---|---|---|
| Mosquitoes (wingbeat) | CNN on mel spectrogram | 60–75% | Moderate |
| Mosquitoes (wingbeat) | Optical IR sensor | >90% | Good |
| Crickets / grasshoppers | CNN on spectrogram | >90% | Good |
| Moths / flies (wingbeat) | CNN on mel spectrogram | <60% | Early |
| Multi-species flight | Scheimpflug lidar | High | Research only |

---

## 3. The IR Optical Sensor Signal

### 3.1 What the Sensor Produces

An IR LED + photodiode pair does not detect sound — it produces a **time-series amplitude
signal**. As the insect's wings sweep through the beam they partially block it, creating a
modulation pattern that encodes:

- Wingbeat frequency (fundamental Hz)
- Harmonic structure (ratio of harmonics is species-informative)
- Modulation depth (related to wing size and stroke amplitude)
- Inter-pulse timing

The signal is mathematically similar to what a microphone would pick up from a wingbeat,
but with far better signal-to-noise ratio because there is no noise propagation path.

### 3.2 Classification Pipeline

```
IR photodiode signal (raw time-series)
        │
        ▼
FFT / Short-Time Fourier Transform
        │
        ▼
Spectrogram or feature vector
(WBF, harmonics, modulation depth)
        │
        ▼
Classifier
```

### 3.3 Models Used

| Approach | Notes |
|---|---|
| CNN on spectrogram | Most common modern approach — treat IR signal like audio |
| SVM / Random Forest on features | Older but still used; interpretable, works well for distinct WBF |
| LSTM / Transformer on raw time-series | Captures temporal structure without FFT pre-processing |
| 1D CNN on raw signal | Lightweight; suitable for embedded NPU deployment |

Because the IR modulation signal is mathematically similar to an acoustic wingbeat signal,
models trained on acoustic data can often be adapted for IR signals with minimal
modification. The IR signal simply has better SNR.

---

## 4. The Role of the FFT

The FFT (Fast Fourier Transform) converts the signal from the **time domain** to the
**frequency domain**.

### 4.1 What It Does

The raw IR signal in the time domain shows amplitude over time — something repeating, but
it is hard to quantify. After FFT, you see energy at each frequency directly:

```
energy
    │
    │        █
    │        █
    │        █     █
    │   █    █     █
    └────────────────── Hz
         200  400   800
```

The dominant frequency (400 Hz here) is the wingbeat fundamental; 800 Hz is the second
harmonic. Both are immediately readable.

### 4.2 Why Harmonics Matter

Different insects have characteristic wingbeat frequencies:

| Insect | Approximate WBF |
|---|---|
| Hawk-moth | ~25 Hz |
| Honeybee | ~230 Hz |
| Blowfly | ~150 Hz |
| *Aedes* mosquito | ~400–600 Hz |

The **ratio between harmonics** is often more discriminative than the fundamental frequency
alone — two species may share a similar WBF but have very different harmonic profiles, and
the FFT reveals both simultaneously.

### 4.3 Why Use FFT Rather Than Raw Time-Series

- Makes periodic structure **explicit** rather than something the model must learn to extract
- **Removes time-shift dependency** — it does not matter when in the recording the wingbeat starts
- **Compresses** the signal — 1 second at 8 kHz is 8,000 samples; only the first few hundred
  frequency bins carry useful information
- Makes the classifier's job easier, requiring less training data

LSTM and Transformer models can work directly on raw time-series, but FFT pre-processing
typically yields better sample efficiency for small insect datasets.

---

## 5. Performance with Multiple Insects

This is one of the fundamental problems with the approach.

### 5.1 What Happens Physically

When two insects are in the beam simultaneously, the IR signal is the **sum** of both
wingbeat modulations:

```
Insect A alone:  peaks at 400, 800, 1200 Hz  (clean)
Insect B alone:  peaks at 230, 460, 690  Hz  (clean)
Both together:   peaks at 230, 400, 460, 690, 800, 1200 Hz
                 + cross-modulation artefacts
                 → classifier is confused
```

### 5.2 Degradation by Scenario

| Scenario | FFT Performance |
|---|---|
| One insect at a time | Good |
| Two insects, very different WBF | Detects both frequencies; cannot count individuals |
| Two insects, similar WBF | Effectively blind — peaks merge |
| Three or more insects | Essentially unusable for identification |

### 5.3 Mitigations

**Narrow the beam** — A 1–2 mm beam width in a funnel entrance makes simultaneous
crossings rare. This is the most effective solution and is how most practical systems
handle it.

**Multiple beams** — An array of IR pairs at different positions gives each insect its own
channel.

**Short-time analysis (STFT / spectrogram)** — If insects enter the beam at slightly
different times, temporal separation may be possible even with overlapping frequencies.

**Source separation** — Blind source separation (ICA, NMF) can decompose mixed signals in
theory; unreliable in practice when frequencies overlap.

---

## 6. Fusion with Visual Detection

### 6.1 Complementary Failure Modes

The camera and IR sensor fail in opposite situations, making them naturally complementary:

| Situation | Camera | IR Sensor |
|---|---|---|
| Multiple insects simultaneously | Handles well — each detection independent | Degrades badly |
| Fast movement / motion blur | Struggles | Unaffected |
| Visually similar species | Low confidence | WBF may discriminate |
| Dark conditions | Needs illumination | Works in complete darkness |
| Occluded / partial view | Low confidence | Unaffected |
| Clean single insect | Good ID | Good ID |

### 6.2 Fusion Strategy

The cleanest approach is to use IR as a **confidence booster** rather than a parallel
classifier:

```
Camera → visual ID  → label + confidence (e.g. "Lepidoptera, 0.71")
IR     → WBF + harmonics → acoustic ID → label + confidence (e.g. "Lepidoptera, 0.84")
                                    │
                              Are they consistent?
                              │                    │
                            Yes                    No
                      fuse scores             flag for review
                  ("Lepidoptera, 0.91")    (conflicting signal)
```

When both modalities **agree**, the fused confidence is significantly higher than either
alone. When they **disagree**, this is itself informative — it may indicate a visually
confusable species pair, or a noisy IR crossing.

### 6.3 Temporal Association

For fusion to be valid the two signals must refer to the **same insect**. Positioning the
IR sensor at the trap entrance means the IR event (t = 0) naturally precedes the insect
appearing in the camera frame (t ≈ +50 ms), giving a clean temporal association. The funnel
also serialises insects, eliminating the multiple-simultaneous problem.

### 6.4 Benefits of the Fused Approach

- Removes ambiguous low-confidence visual detections rather than propagating uncertainty
- Enables species-level discrimination between visually similar species if WBF differs
- Provides graceful degradation if one sensor fails
- Enriches metadata: visual confidence + acoustic confidence + agreement flag
- Works in complete darkness for the acoustic channel

---

## 7. Practical Additions for the AI Trap

For a camera-trap style instrument, the most viable acoustic/IR additions are:

1. **IR optical sensor at funnel entrance** — serialises insects, clean single-insect
   signal, no noise problem, cheap (IR LED + photodiode + GPIO)
2. **Contact microphone on landing surface** — detects substrate vibration when insects
   land; short range but noise-resistant
3. **Microphone inside enclosed housing** — if insects are contained, near-field acoustic
   is viable

For embedded deployment on the Luckfox (RV1103 NPU), a **1D CNN or CNN on compact
spectrogram** in RKNN format is the natural fit — lightweight, fast inference, and slots
into the existing post-detection crop classification pipeline.
