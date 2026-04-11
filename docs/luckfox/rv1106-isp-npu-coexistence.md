# RV1106 ISP + NPU Coexistence: One-Shot 3A Convergence

## The problem

The Luckfox Pico Zero (RV1106G3) is designed to run ISP processing (rkaiq AE/AWB/CCM) and
RKNN NPU inference simultaneously — this is its primary use case as an AI camera SoC.
In practice, every `rknn_run()` call times out with irq status `0x0` whenever rkaiq is
active. The NPU never completes inference; the RKNN library prints:

```
E RKNN: failed to submit!, op id: 39,
  op name: Conv:/model.8/.../cv2/conv/Conv,
  task start: 0, task number: 113, run task counter: 82, int status: 0
```

The NPU successfully processes 56–85 of 113 YOLO layers, then stalls permanently until the
6.5-second driver watchdog fires. The failure layer varies (always in model.6/model.8 —
the deepest feature maps with peak DDR footprint), indicating a race condition rather than a
fixed hardware limit.

## Investigation

### What the error means

`int status: 0` means the NPU completion interrupt never fired. The NPU was in the middle of
writing a layer's output activations to DDR and the write never completed. The driver waited
6.5 seconds for the completion interrupt, then declared a timeout.

The NPU task submission protocol requires:

1. NPU computes task N output into its on-chip SRAM
2. NPU DMA engine writes task N output from SRAM to DDR
3. NPU fires completion interrupt (or sets polling bit in bypass mode)
4. Driver submits task N+1 (reads inputs from DDR, writes outputs to DDR)

If step 2 stalls, step 3 never happens, and step 4 is never reached. The NPU is deadlocked
waiting for a DDR bus grant that never comes.

### Hardware bus architecture

The RV1106 AXI fabric is a NIC-400 interconnect. Per-master QoS generators sit between
each bus master and the DDR controller:

```
ISP ─────► [QoS 0xFF130000] ─┐
VICAP ────► [QoS 0xFF130100] ─┤
NPU ──────► [QoS 0xFF140000] ─┤──► MSCH (DDR scheduler) ──► LPDDR4
CPU ──────► [QoS 0xFF110000] ─┘
```

This suggested an obvious fix: raise NPU priority, lower ISP priority.

### NIC-400 QoS attempt

The real QoS register addresses were found in `arch/arm/mach-rockchip/rv1106/rv1106.c`
from the Luckfox U-Boot source (not from the TRM, which does not document these):

| Master | Priority register | Default |
|--------|------------------|---------|
| ISP    | `0xFF130008`     | `0x303` (priority 3/3) |
| VICAP  | `0xFF130108`     | `0x303` |
| NPU    | `0xFF140008`     | `0x303` |

> **Note:** Earlier attempts used wrong addresses (`0xFF018004`, `0xFF050018`) that were GRF
> status/routing registers, not QoS registers.

Setting ISP and VICAP to priority 0, NPU to priority 7 (fixed arbitration mode) had
**no effect whatsoever**. The NPU continued to time out at exactly the same layers.

### Why QoS had no effect

Further research confirmed two reasons:

**1. The MSCH (Memory Scheduler Controller) is HPMCU-managed.**
The RV1106 DDR controller is initialised entirely by a closed binary blob
(`rv1106_ddr_924MHz_v1.15.bin`) and the HPMCU (an on-chip RISC-V microcontroller)
manages DDR QoS policy at runtime. There is no DT node for the DDR controller; no
`rockchip,rv1106-dmc` driver exists in the kernel. The MSCH base address is not
published. Writes to the candidate address `0xFF800000` (probable DDR block) did not
stick. **The DDR arbiter is not accessible from Linux userspace.**

**2. rkaiq's DDR load is not video frame bandwidth.**
The ISP video output at 640×480 × 30fps NV12 is ~14 MB/s — only ~1% of available DDR
bandwidth. That alone cannot explain a 6.5-second stall. The actual source of DDR
contention is **rkaiq's internal algorithm threads**:

- Stats DMA: every frame, rkaiq reads ISP AE histograms, AWB per-zone colour data,
  and AF sharpness statistics from DDR
- Parameter DMA: rkaiq writes updated ISP parameters (3D-LUT, CCM matrix, AWB gains,
  gamma table, denoise coefficients) back to ISP hardware registers via DDR
- Algorithm memory: rkaiq's CPU threads do heavy DDR access (cache misses) running
  the AE/AWB/CCM algorithms, loading calibration data, and managing its IPC server

This combined DDR pressure from multiple threads, all touching different memory regions,
creates frequent DDR bus contention that starves the NPU's burst DMA. The ISP/stats DMA
path likely has a dedicated high-priority DDR port that the NIC-400 QoS generator does
not control.

This was confirmed by comparison: running VI at **1920×1080** (93 MB/s) *without* rkaiq
causes **no NPU timeouts**. Running VI at **640×480** (14 MB/s) *with* rkaiq causes
**100% NPU timeouts**. The video bandwidth was irrelevant; rkaiq's presence was the cause.

### Confirmed by library inspection

```
$ ldd /oem/usr/lib/librkaiq.so
    libstdc++.so.6, libgcc_s.so.1, libc.so.0
```

`librkaiq.so` does not depend on `librknnmrt.so`. rkaiq does not use the NPU internally.
This ruled out rkaiq competing directly for NPU resources.

## The fix: one-shot 3A convergence

### Concept

rkaiq's job is to:
1. Observe the scene (read ISP statistics every frame)
2. Compute optimal exposure, white balance, and colour correction (run 3A algorithms)
3. Apply the result to the ISP hardware registers (write ISP parameters)

Once AE and AWB have *converged* (locked onto the scene), steps 1–3 produce essentially
the same result every frame. The ISP hardware holds the correct settings. rkaiq's ongoing
work is redundant until the scene changes.

The key insight: **ISP hardware registers are persistent**. Sensor exposure and gain are
stored in the sensor's own registers. The ISP's AWB gain matrix, CCM coefficients, gamma
LUT, and all other image-quality parameters sit in hardware registers that retain their
values when software stops writing to them.

The `rk_aiq_uapi2_sysctl_stop()` API takes a `keep_ext_hw_st` parameter:
- `false`: reset the ISP and sensor to default state on stop
- `true`: leave the ISP hardware and sensor registers exactly as they are on stop

### Implementation

```
Boot
 │
 ▼
RKNN context initialised (NPU DMA buffers allocated)
 │
 ▼
rkaiq sysctl_init()  ─── loads IQ calibration file, opens ISP stats paths
 │
 ▼
rkaiq sysctl_prepare(640×480)  ─── configures ISP pipeline
 │
 ▼
rkaiq sysctl_start()  ─── 3A loops begin; ISP gets AE/AWB/CCM applied
 │
 ▼
sleep(5 seconds)  ─── AE converges (~1–2 s), AWB converges (~2–3 s)
 │
 ▼
rkaiq sysctl_stop(keep_ext_hw_st=true)  ─── rkaiq threads exit, ISP state PRESERVED
rkaiq sysctl_deinit()
 │
 │  At this point:
 │  • Sensor registers: exposure time, analogue gain → calibrated values, held
 │  • ISP AWBGAIN registers: R/Gr/Gb/B multipliers → calibrated, held
 │  • ISP CCM matrix: colour correction coefficients → calibrated, held
 │  • ISP gamma LUT: tone mapping curve → calibrated, held
 │  • rkaiq algorithm threads: STOPPED, DDR no longer accessed
 │
 ▼
RK_MPI_SYS_Init() → VI_EnableChn() → VPSS → VENC
 │  (ISP runs normally, outputting calibrated frames to rkcif → VI)
 │
 ▼
NPU inference runs  ─── zero DDR contention from rkaiq ✓
```

### Code

In `firmware/main_rkmpi.cpp`, after `rk_aiq_uapi2_sysctl_start()` succeeds:

```cpp
static const int RKAIQ_CONVERGE_SECS = 5;
printf("  rkaiq started — waiting %ds for AE/AWB/CCM to converge...\n",
       RKAIQ_CONVERGE_SECS);
std::this_thread::sleep_for(std::chrono::seconds(RKAIQ_CONVERGE_SECS));

XCamReturn stopRet = rk_aiq_uapi2_sysctl_stop(
    aiqCtx, /*keep_ext_hw_st=*/true);

rk_aiq_uapi2_sysctl_deinit(aiqCtx);
aiqCtx = nullptr;   // skip stop in shutdown path

printf("  rkaiq stopped — ISP holds calibrated AE/AWB/CCM/gamma\n");

// ISP handles WB in hardware — disable software correction
camCfg.wbR = camCfg.wbG = camCfg.wbB = 1.0f;

// Restore full capture resolution now that rkaiq DDR load is gone.
// VI alone at 1920x1080 does not starve the NPU (confirmed by --no-rkaiq).
camCfg.captureWidth  = 1920;
camCfg.captureHeight = 1080;
```

### Results

After the fix, on a clean boot via the systemd-equivalent S99trap init script:

```
  sensor entity : m00_b_imx415 4-0037
  rkaiq started — running AE/AWB/CCM convergence...
  waiting 5s for AE/AWB/CCM to converge...
  rkaiq stopped — ISP holds calibrated AE/AWB/CCM/gamma; NPU DDR conflict eliminated
  software WB correction disabled (ISP hardware holds AWB gains)
  capture resolution restored to 1920x1080

  [rkmpi] pipeline ready  VI 1920x1080 → VPSS inf=320×320 str=640×480(VENC) full=1920×1080
  Running — Ctrl+C to stop

  [rkmpi] frames=643  dropped=112
    infer_queue_dropped=249
    DB rows=0  tracks=0  size=0.1 MB
```

Zero `rknn_run` failures. Zero NPU timeouts. Continuous YOLO11n inference at 1920×1080
capture with hardware ISP calibration.

## Trade-offs and future work

### Static ISP calibration

After convergence, AE and AWB are frozen. The image quality will not adapt to:
- Gradual light changes (clouds passing, sun angle shifting)
- Scene transitions (camera moved to a different location)
- Day/night transitions (would require separate IQ tuning anyway)

For a fixed-position wildlife camera trap in an outdoor environment with relatively stable
ambient lighting, this is acceptable. The 5-second convergence window at startup is sufficient
for AE and AWB to lock to the prevailing conditions.

### Periodic re-calibration

If scene conditions change significantly, yolo_rkmpi can be restarted (via the watchdog or
systemctl) to trigger a fresh 5-second convergence window. A more sophisticated approach
would periodically pause inference, re-run rkaiq for a few seconds, stop it again, and resume.
This is not currently implemented.

### Convergence time

5 seconds is conservative for outdoor daylight. In low-light conditions AE may need more
time (the sensor must integrate longer frames to get good statistics). A future improvement
could monitor rkaiq's AE convergence status via `rk_aiq_uapi2_AE_getLockStatus()` rather
than using a fixed sleep.

## Register addresses for reference

These were investigated during the root-cause analysis and are documented here for future work.

### NIC-400 QoS generators (writable from Linux via /dev/mem)

| Master | Priority reg | Shaping base | Notes |
|--------|-------------|--------------|-------|
| ISP    | `0xFF130008` | `0xFF130180` | default 0x303; shaping not set by U-Boot |
| VICAP  | `0xFF130108` | `0xFF130200` | default 0x303 |
| NPU    | `0xFF140008` | `0xFF140080` | default 0x303; U-Boot sets NBPKTMAX=4 at `0xFF140088` |
| VENC   | `0xFF150008` | `0xFF150100` | U-Boot sets to 0x303 |

QoS priority register format: `bits[19:16]` = read priority, `bits[3:0]` = write priority.
Value `0x303` = priority 3 read/write. Max = `0xF000F` = priority 15 read/write.

**Untried but potentially effective:** `SHAPING_NBPKTMAX` at ISP shaping base + 0x008
(`0xFF130188`) limits ISP max in-flight DDR transactions. U-Boot constrains the NPU to 4
packets but leaves ISP unconstrained. Setting ISP NBPKTMAX to a small value (e.g. 2) would
force the ISP to yield DDR more frequently, potentially without the need for one-shot
convergence. This was superseded by the one-shot approach before it could be tested.

### MSCH / DDR controller (not accessible from Linux)

The MSCH (Memory Scheduler Controller) is initialised by `rv1106_ddr_924MHz_v1.15.bin`
and managed at runtime by the HPMCU (on-chip RISC-V microcontroller). No base address is
published. The HPMCU firmware changelog entries confirm DDR QoS changes were made in
firmware updates (`rv1106_hpmcu_wrap_v1.50`: "increase encoder ddr qos";
`rv1106_hpmcu_wrap_v1.53`: "adjust QOS policy"). These changes are not accessible from
Linux userspace.

The RV1126 (closest relative with open-source DDR driver) places its MSCH at `0xFE800000`;
the RV1106 equivalent is in an undocumented region of the `0xFF...` peripheral map.
