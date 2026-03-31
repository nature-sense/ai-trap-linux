# ai-trap-gstr — Claude context

## Hardware
- Target: Waveshare CM5-NANO-A (production), CM5-IO-BASE-A (test)
- Camera: IMX708 on **cam1**
- Storage: eMMC (no SD card) — flashed via Raspberry Pi Imager + rpiboot from Mac
- OS: Raspberry Pi OS / Debian Trixie 64-bit Lite (aarch64)

## Devices
- `trap006` → `trap@trap006.local` (CM5-IO-BASE-A)
- `trap001` → `trap@trap001.local` / `trap@10.0.10.247` (Waveshare Nano Base Board A, WiFi only)

## Known issues

### CM5 reboot loop — static dtoverlay at boot
**Symptom:** Board reboots in a loop immediately after a fresh install and reboot.

**Root cause (confirmed):** Loading `dtoverlay=imx708,cam1` statically via `/boot/firmware/config.txt`
causes a power spike that hard-resets the CM5. The fix is to load the overlay at runtime via
`camera-overlay.service` instead. Do NOT add `dtoverlay=imx708` to `config.txt`.

**Fix:** Remove `dtoverlay=imx708` from `/boot/firmware/config.txt`:
- The FAT32 boot partition mounts natively on Mac via rpiboot — edit `config.txt` directly in Finder.
- Then redeploy the package so `camera-overlay.service` is installed and enabled.

**Architecture:** `camera-overlay.service` runs `dtoverlay imx708` after `network-online.target`
(by which point the board power rails are stable — avoids the boot power spike on Waveshare Nano).
`ai-trap.service` has `After=camera-overlay.service Wants=camera-overlay.service`.
`install.sh` actively removes any `dtoverlay=imx708` line found in `config.txt`.

### CM5 reboot loop — double overlay load (older deployments)
**Symptom:** Board reboots in a loop after updating an older package.

**Cause:** Old `camera-overlay.service` loaded the overlay at runtime AND old `install.sh` also
injected `dtoverlay=imx708,cam1` into `config.txt`. Double-loading crashes CM5.

**Fix (SSH access):**
```bash
sudo systemctl disable --now camera-overlay.service
sudo rm /etc/systemd/system/camera-overlay.service
sudo systemctl daemon-reload && sudo reboot
```
Then redeploy the current package.

### Kernel panic / hard reset during inference
- `capture_width=2304, capture_height=1296` causes kernel hard-reset on CM5 (35 MB ncnn mat saturates memory bandwidth)
- Use `1536x864` — stable on CM5-IO-BASE-A

### Waveshare Nano Base Board A — thermal crash during inference
**Symptom:** Board runs stably for ~5 minutes then hard-resets while ai-trap is capturing + running
inference. Repeats every ~5 minutes. No kernel panic log survives (volatile journal).

**Root cause:** The Waveshare Nano runs very hot under sustained camera + YOLO inference load.
A passive heatsink alone is insufficient to keep the CM5 cool enough. Eventually the board
hard-resets (power or thermal protection trip).

**Fix — reduce thermal load in `trap_config.toml`:**
```toml
[camera]
framerate    = 10   # was 30 — cuts camera DMA + inference frequency by 3×
buffer_count = 2    # was 4  — halves DMA buffer memory
```

**Optional — cap CPU frequency in `/boot/firmware/config.txt`:**
```
arm_freq=1500   # default max ~2400 MHz; capping reduces heat at cost of inference speed
```

**Note:** This crash is distinct from the boot-time power spike issue. Boot overlay load works
correctly (`After=network-online.target`). The crash only occurs after sustained AI inference.
Trap006 (CM5-IO-BASE-A) does not exhibit this — it has better thermal mass and power delivery.

### libcamera IPA kernel panic
- Bundled IPA modules cause kernel panic on CM5
- Service sets `LIBCAMERA_IPA_MODULE_PATH` and `LIBCAMERA_IPA_PROXY_PATH` to system paths
- Requires `libcamera0.7` and `libcamera-ipa` installed from the Raspberry Pi apt repo

### StartLimitIntervalSec in wrong section
- Must be in `[Unit]`, not `[Service]` — systemd on Trixie ignores it in `[Service]`
- If misplaced, default start limit (5 attempts / 10s) applies and service stops retrying
