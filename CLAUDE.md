# ai-trap-gstr — Claude context

## Hardware
- Target: Waveshare CM5-NANO-A (production), CM5-IO-BASE-A (test)
- Camera: IMX708 on **cam1**
- Storage: eMMC (no SD card) — flashed via Raspberry Pi Imager + rpiboot from Mac
- OS: Raspberry Pi OS / Debian Trixie 64-bit Lite (aarch64)

## Devices
- `trap006` → `trap@trap006.local`

## Known issues

### CM5 reboot loop — static dtoverlay at boot
**Symptom:** Board reboots in a loop immediately after a fresh install and reboot.

**Root cause (confirmed):** Loading `dtoverlay=imx708,cam1` statically via `/boot/firmware/config.txt`
causes a power spike that hard-resets the CM5. The fix is to load the overlay at runtime via
`camera-overlay.service` instead. Do NOT add `dtoverlay=imx708` to `config.txt`.

**Fix:** Remove `dtoverlay=imx708` from `/boot/firmware/config.txt`:
- The FAT32 boot partition mounts natively on Mac via rpiboot — edit `config.txt` directly in Finder.
- Then redeploy the package so `camera-overlay.service` is installed and enabled.

**Architecture:** `camera-overlay.service` runs `dtoverlay imx708,cam1` after `sysinit.target`.
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
- Use `1536x864` — stable

### libcamera IPA kernel panic
- Bundled IPA modules cause kernel panic on CM5
- Service sets `LIBCAMERA_IPA_MODULE_PATH` and `LIBCAMERA_IPA_PROXY_PATH` to system paths
- Requires `libcamera0.7` and `libcamera-ipa` installed from the Raspberry Pi apt repo

### StartLimitIntervalSec in wrong section
- Must be in `[Unit]`, not `[Service]` — systemd on Trixie ignores it in `[Service]`
- If misplaced, default start limit (5 attempts / 10s) applies and service stops retrying
