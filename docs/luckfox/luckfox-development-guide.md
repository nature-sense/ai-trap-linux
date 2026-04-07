# Luckfox Pico Zero — Development Guide

End-to-end guide covering initial hardware setup, OS flashing, software
installation, WiFi provisioning, and iterative development with CLion on macOS.

---

## Contents

1. [How it works](#1-how-it-works)
2. [Prerequisites](#2-prerequisites)
3. [Quick start — full setup in one command](#3-quick-start)
4. [Step-by-step setup](#4-step-by-step-setup)
   - [4.1 Firmware management](#41-firmware-management)
   - [4.2 Flash the OS](#42-flash-the-os)
   - [4.3 Install ai-trap software](#43-install-ai-trap-software)
   - [4.4 Configure WiFi](#44-configure-wifi)
5. [Verify and control the service](#5-verify-and-control-the-service)
6. [GitHub Releases](#6-github-releases)
7. [Build environment setup](#7-build-environment-setup)
8. [CLion setup](#8-clion-setup)
9. [Daily development workflow](#9-daily-development-workflow)
10. [Project structure](#10-project-structure)
11. [CLion tips](#11-clion-tips)
12. [Runtime configuration](#12-runtime-configuration)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. How it works

The Luckfox Pico Zero runs a minimal Linux (busybox/uClibc) on an RV1106G3
(Cortex-A7) SoC. Two aspects of the platform shape the workflow:

**USB networking:** macOS has no native RNDIS driver so the Luckfox's USB
Ethernet gadget does not appear as a network interface. **ADB** (Android Debug
Bridge) is used for all initial setup — flashing verification, software
installation, and WiFi provisioning. Once the board is on your WiFi network,
all subsequent access is via SSH.

**Cross-compilation:** The Luckfox toolchain
(`arm-rockchip830-linux-uclibcgnueabihf-g++`) is an x86_64 Linux ELF binary
and cannot run natively on macOS. Builds run inside a Docker container
(`ubuntu:22.04 linux/amd64`); Docker Desktop handles Rosetta 2 translation on
Apple Silicon automatically.

### Scripts overview

All setup steps are scripted. Each script installs its own prerequisites
(Homebrew, rkdeveloptool, adb) automatically if they are missing or outdated.

| Script | Purpose |
|---|---|
| `luckfox-firmware-fetch.sh` | Manage OS firmware in `luckfox/system/` |
| `luckfox-flash.sh` | Stage 1 — flash OS via rkdeveloptool |
| `luckfox-install.sh` | Stage 2 — push ai-trap software via ADB |
| `luckfox-wifi-setup.sh` | Stage 3 — configure WiFi credentials via ADB |
| `luckfox-setup.sh` | Orchestrator — runs all stages end-to-end |
| `build-luckfox-mac.sh` | Docker cross-compile (development builds) |
| `clion-compile-commands.sh` | Generate CLion compilation database |

### Installation flow

```
luckfox-setup.sh
  │
  ├─ luckfox-flash.sh        Maskrom → rkdeveloptool → OS on eMMC → reboot
  │
  ├─ luckfox-install.sh      ADB → push binary + models + config + init scripts
  │       (downloads latest GitHub Release, or uses local build if present)
  │
  ├─ luckfox-wifi-setup.sh   ADB → wpa_supplicant → udhcpc → persist on boot
  │
  └─ reboot                  Board comes up on WiFi, services start automatically
```

### Development flow

```
build-luckfox-mac.sh       Docker build  → build-luckfox/yolo_v4l2
                                            build-luckfox/compile_commands.json
clion-compile-commands.sh  Path remap    → build-clion/compile_commands.json
CLion                      CompDB project  firmware/src/** fully indexed
```

---

## 2. Prerequisites

All tools are installed automatically by the scripts when needed. For reference:

| Tool | Installed by |
|---|---|
| Homebrew | All scripts (auto-installs if missing) |
| rkdeveloptool | `luckfox-flash.sh` (via Homebrew tap) |
| Android Platform Tools (adb) | `luckfox-install.sh`, `luckfox-wifi-setup.sh` |
| Docker Desktop | Manual — https://www.docker.com/products/docker-desktop/ |
| CLion | Manual — https://www.jetbrains.com/clion/ |

Docker Desktop and CLion are only needed for development builds and CLion
code intelligence. They are not required for end-user installation.

---

## 3. Quick start

To go from a brand-new board to a running insect trap in a single command:

### 3.1 Import the OS firmware (once)

Download the latest `Luckfox_Pico_Zero_EMMC_YYMMDD` folder from:
**https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads/**

Then import it into the project:

```bash
bash scripts/luckfox-firmware-fetch.sh --import ~/Downloads/Luckfox_Pico_Zero_EMMC_250802
```

### 3.2 Run the full setup

```bash
bash scripts/luckfox-setup.sh "YourWiFiSSID" "YourWiFiPassword"
```

The script will:
1. Prompt you to put the board into Maskrom mode (instructions printed)
2. Flash the OS firmware
3. Wait for the board to boot
4. Install ai-trap software via ADB (downloads latest GitHub Release automatically)
5. Configure WiFi via ADB
6. Reboot the board into production mode
7. Print the SSH command to use from this point on

**Total time:** ~5 minutes (excluding firmware download).

> **Use a data-capable USB-C cable.** Charge-only cables have no data lines —
> the board will be completely invisible with no error. A cable that works for
> file transfer with a phone is suitable.

> **Use your 2.4 GHz WiFi SSID** — 5 GHz may be unreliable on this firmware.

---

## 4. Step-by-step setup

Use these sections if you want to run stages individually or understand what
each script does.

### 4.1 Firmware management

The OS firmware is stored in `luckfox/system/` (gitignored — too large for the
repo). Use `luckfox-firmware-fetch.sh` to manage it.

**Check current status:**
```bash
bash scripts/luckfox-firmware-fetch.sh
```

**Import a downloaded firmware directory:**
```bash
bash scripts/luckfox-firmware-fetch.sh --import /path/to/Luckfox_Pico_Zero_EMMC_250802
```

The script validates that all required partition images are present
(`download.bin`, `idblock.img`, `uboot.img`, `boot.img`, `rootfs.img`, etc.)
and copies the directory into `luckfox/system/`. The most recently dated
firmware is used automatically by `luckfox-flash.sh`.

**Firmware directory contents:**
```
Luckfox_Pico_Zero_EMMC_250802/
├── download.bin    ← DDR loader (required by rkdeveloptool)
├── env.img         ← partition 1 — U-Boot environment
├── idblock.img     ← partition 2 — idbloader
├── uboot.img       ← partition 3 — U-Boot
├── boot.img        ← partition 4 — kernel + DTB
├── oem.img         ← partition 5 — OEM data
├── userdata.img    ← partition 6 — userdata
├── rootfs.img      ← partition 7 — root filesystem
└── update.img      ← packed image (not used — rkdeveloptool uses individual files)
```

### 4.2 Flash the OS

Put the board into **Maskrom mode** before running:

1. Unplug the USB cable from the board
2. Hold the **BOOT** button
3. While holding BOOT, plug the USB-C cable into the board and your Mac
4. Hold for 1–2 seconds, then release

Verify the board is detected (macOS shows it as an "unknown device"):
```bash
system_profiler SPUSBDataType 2>/dev/null | grep -B2 -A8 "0x2207"
# Look for: USB Vendor ID: 0x2207 / USB Product ID: 0x110c
```

Then flash:
```bash
bash scripts/luckfox-flash.sh
```

The script installs `rkdeveloptool` automatically if not present (via the
`IgorKha/homebrew-rkdeveloptool` Homebrew tap). It writes all seven partitions
at their correct eMMC sector addresses and reboots the board.

> **Why rkdeveloptool and not upgrade_tool?** The Rockchip `upgrade_tool`
> fails on macOS APFS with `ftruncate: Invalid argument`. Use rkdeveloptool.
> The `update.img` packed format is also not compatible with `rkdeveloptool wl`
> — the script writes individual partition images at the addresses from
> `sd_update.txt`.

**Partition addresses (from sd_update.txt):**

| Image | Sector |
|---|---|
| `env.img` | `0x0` |
| `idblock.img` | `0x40` |
| `uboot.img` | `0x440` |
| `boot.img` | `0x640` |
| `oem.img` | `0x10640` |
| `userdata.img` | `0x110640` |
| `rootfs.img` | `0x190640` |

**Fallback — browser WebUSB tool (no install needed):**
Open **https://asadmemon.com/rkdeveloptool/** in Chrome with the board in
Maskrom mode. Select `download.bin` as the loader and `update.img` as the
image.

### 4.3 Install ai-trap software

Once the board has booted after flashing, ADB becomes available. Run:

```bash
bash scripts/luckfox-install.sh
```

The script:
1. Waits for an ADB device to appear (up to 60 s)
2. Selects the software source:
   - **Local build** — uses `build-luckfox/yolo_v4l2` if present (from `build-luckfox-mac.sh`)
   - **GitHub Release** — downloads the latest `ai-trap-luckfox-vX.Y.Z.tar.gz` automatically
3. Pushes via ADB to the device:

| File | Device path |
|---|---|
| `yolo_v4l2` | `/opt/trap/yolo_v4l2` |
| `model.ncnn.param` | `/opt/trap/model.ncnn.param` |
| `model.ncnn.bin` | `/opt/trap/model.ncnn.bin` |
| `trap_config.toml` | `/opt/trap/trap_config.toml` |
| `S50wifi` | `/etc/init.d/S50wifi` |
| `S99trap` | `/etc/init.d/S99trap` |

Force a specific source if needed:
```bash
bash scripts/luckfox-install.sh --local    # use local build only
bash scripts/luckfox-install.sh --release  # download GitHub Release
```

### 4.4 Configure WiFi

```bash
bash scripts/luckfox-wifi-setup.sh "YourSSID" "YourPassword"
# or omit arguments to be prompted interactively
bash scripts/luckfox-wifi-setup.sh
```

The script:
1. Waits for an ADB device
2. Writes credentials to `/etc/wpa_supplicant.conf`
3. Starts `wpa_supplicant` and verifies association
4. Runs `udhcpc` to obtain an IP address
5. Writes `/etc/network/interfaces` so WiFi starts on every boot
6. Prints the SSH command to use from this point on

After this step the USB cable is no longer needed. SSH to the printed IP:
```bash
ssh root@<ip>
```

---

## 5. Verify and control the service

### Check status and logs

```bash
ssh root@<luckfox-ip>
/etc/init.d/S99trap status
tail -f /var/log/ai-trap.log
```

### Service control

```bash
/etc/init.d/S50wifi start     # start WiFi manager
/etc/init.d/S50wifi stop
/etc/init.d/S99trap start     # start ai-trap
/etc/init.d/S99trap stop
/etc/init.d/S99trap restart
```

Both services start automatically on boot via `/etc/init.d/`.

### Verify camera

```bash
ls /dev/video*          # should show /dev/video0
v4l2-ctl --list-devices
```

### Configure the trap

Trap ID, location, and thresholds can be updated via the REST API after
first boot without re-flashing:

```bash
# Read current config
curl http://<luckfox-ip>:8080/api/config

# Update trap ID and location
curl -X POST http://<luckfox-ip>:8080/api/config \
     -H 'Content-Type: application/json' \
     -d '{"trap":{"id":"trap_007","location":"Garden north"}}'
```

---

## 6. GitHub Releases

Tagged releases are built automatically by GitHub Actions and published as
GitHub Releases containing a self-contained installation bundle:

```
ai-trap-luckfox-v1.0.0.tar.gz
├── yolo_v4l2               application binary (stripped ARMv7)
├── model.ncnn.param        NCNN model
├── model.ncnn.bin          NCNN model weights
├── trap_config.toml        production default configuration
├── S50wifi                 WiFi AP/station init script
├── S99trap                 ai-trap service init script
└── VERSION                 release tag
```

`luckfox-install.sh` downloads this bundle automatically when no local build
is present. To cut a new release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

GitHub Actions builds, packages, and publishes the release within ~5 minutes.

---

## 7. Build environment setup

Only needed for making code changes. Not required for end-user installation.

### 7.1 Install Docker Desktop

Download from https://www.docker.com/products/docker-desktop/ and install.
Docker Desktop handles the `ubuntu:22.04` container and Rosetta 2 translation
on Apple Silicon automatically.

### 7.2 First build

```bash
bash scripts/build-luckfox-mac.sh
```

On first run (~20 minutes) Docker downloads the Luckfox toolchain and
cross-compiles SQLite3 and NCNN. Everything is cached in
`~/.cache/ai-trap/luckfox/` — subsequent builds take ~60–90 seconds.

Outputs:
```
build-luckfox/yolo_v4l2               stripped ARMv7 binary
build-luckfox/compile_commands.json   compilation database (Docker-internal paths)
```

After building, `luckfox-install.sh` will automatically prefer the local
binary over downloading a GitHub Release.

---

## 8. CLion setup

### 8.1 Generate the compilation database

```bash
bash scripts/clion-compile-commands.sh
```

Remaps Docker-internal paths (`/src/` → repo root, `/cache/` →
`~/.cache/ai-trap/luckfox/`) and writes `build-clion/compile_commands.json`
with real Mac paths. Must be run after `build-luckfox-mac.sh`.

### 8.2 Open in CLion

1. **File → Open** → navigate to `build-clion/` → **Open**
2. Choose **Open as Project** when prompted
3. CLion indexes `firmware/src/` against the cross-compilation flags

Indexing takes 1–2 minutes on first open. All headers resolve correctly
including sysroot and NCNN headers at `~/.cache/ai-trap/luckfox/sysroot/`.

### 8.3 When to re-run the scripts

Re-run both scripts when adding/removing source files or changing build flags.
For code-only changes, just re-run the build — the compilation database is
unchanged.

```bash
bash scripts/build-luckfox-mac.sh
bash scripts/clion-compile-commands.sh
# CLion prompts "Compilation database has changed — reload?" → click Reload
```

---

## 9. Daily development workflow

```bash
# 1. Edit source files in CLion  (firmware/src/**/*.cpp / *.h)

# 2. Rebuild  (~60–90 s)
bash scripts/build-luckfox-mac.sh

# 3. Push binary and restart
scp build-luckfox/yolo_v4l2 root@<luckfox-ip>:/opt/trap/yolo_v4l2
ssh root@<luckfox-ip> "/etc/init.d/S99trap restart"

# 4. Watch logs
ssh root@<luckfox-ip> "tail -f /var/log/ai-trap.log"
```

---

## 10. Project structure

```
luckfox/
└── system/               OS firmware images — gitignored, managed by
                          luckfox-firmware-fetch.sh

package/luckfox/
├── trap_config.toml      production default config (installed to /opt/trap/)
├── S50wifi               WiFi AP/station manager init script
├── S99trap               ai-trap service init script
└── install.sh            legacy device-side installer (SSH-based, kept for reference)

firmware/
└── src/
    ├── common/           config_loader.h, trap_events.h,
    │                     wifi_manager, ble_gatt_server, epaper_display
    ├── io/               v4l2_capture, rknn_infer
    ├── pipeline/         decoder, tracker, crop_saver,
    │                     exif_writer, persistence, sync_manager
    └── server/           http_server, sse_server, mjpeg_streamer

firmware/models/yolo11n-320/   NCNN model files (committed to repo)
cross/                         Meson cross-file and toolchain config
build-luckfox/                 Docker build output (gitignored)
build-clion/                   CLion CompDB project (.idea/ committed)

scripts/
├── luckfox-firmware-fetch.sh  manage OS firmware in luckfox/system/
├── luckfox-flash.sh           Stage 1 — flash OS via rkdeveloptool
├── luckfox-install.sh         Stage 2 — push software via ADB
├── luckfox-wifi-setup.sh      Stage 3 — configure WiFi via ADB
├── luckfox-setup.sh           orchestrator — all stages end-to-end
├── build-luckfox-mac.sh       Docker cross-compile
└── clion-compile-commands.sh  path remap for CLion

.github/workflows/
└── build-luckfox.yml          CI — builds binary; publishes GitHub Release on v* tags
```

---

## 11. CLion tips

### Navigation

- **⌘+Click** — jump to definition across files
- **⌘E** — Recent Files
- **⌘O** — Navigate → Class
- **⌘⇧F** — Find in Files; set Directory filter to `firmware/src`

### Code style

4-space indentation, K&R brace style. Clang-Format is disabled; CLion's
formatter is configured in `build-clion/.idea/editor.xml` to match the
codebase.

### Header resolution

All headers resolve fully including sysroot and NCNN headers located at
`~/.cache/ai-trap/luckfox/sysroot/usr/include/`. If CLion shows unresolved
includes after a clean checkout, run `build-luckfox-mac.sh` once to populate
the sysroot cache, then regenerate the compilation database.

### Remote debugging with GDB

The binary is ARMv7 — it cannot run on the Mac. For interactive debugging:

```bash
# On the Luckfox
gdbserver :2345 /opt/trap/yolo_v4l2

# CLion: Run → Edit Configurations → + → GDB Remote Debug
#   'target remote' args : <luckfox-ip>:2345
#   Symbol file          : build-luckfox/yolo_v4l2
#   Path mappings        : /opt/trap → <repo>/build-luckfox
```

For most issues, `tail -f /var/log/ai-trap.log` is faster.

---

## 12. Runtime configuration

`package/luckfox/trap_config.toml` is the production config pushed to
`/opt/trap/trap_config.toml` on the device. It uses absolute paths and
enables managed WiFi.

`trap_config.toml` in the repository root is the development config — uses
relative paths and disables managed WiFi so the OS handles networking normally.

Key differences:

| Setting | Production (`package/luckfox/`) | Development (repo root) |
|---|---|---|
| `model.param` | `/opt/trap/model.ncnn.param` | `../firmware/models/...` |
| `wifi.managed` | `true` | `false` |
| `database.path` | `/opt/trap/detections.db` | `detections.db` |
| `crops.output_dir` | `/opt/trap/crops` | `crops` |

Update the trap ID and location via the API after installation — no need to
re-flash or edit files directly:
```bash
curl -X POST http://<luckfox-ip>:8080/api/config \
     -H 'Content-Type: application/json' \
     -d '{"trap":{"id":"trap_007","location":"Garden north"}}'
```

---

## 13. Troubleshooting

### Hardware / flashing

| Symptom | Action |
|---|---|
| Board shows `USB Product ID: 0x110c` but rkdeveloptool doesn't see it | Run `sudo rkdeveloptool ld` — it requires `sudo` |
| Board not found by grep in Maskrom mode | macOS shows it as "unknown device" — look for `USB Vendor ID: 0x2207 / Product ID: 0x110c`; it will not say "Rockchip" |
| Board invisible in Maskrom mode — nothing in System Profiler | Cable is charge-only — replace with a data-capable USB-C cable |
| `upgrade_tool` fails with `ftruncate: Invalid argument` | Known macOS APFS incompatibility — use `luckfox-flash.sh` (rkdeveloptool) instead |
| Board stays in Maskrom after flash | `update.img` was written with `wl 0` — incorrect format; use individual partition images at correct sector addresses (handled automatically by `luckfox-flash.sh`) |
| `rkdeveloptool` not found after `brew install` | Try `brew tap IgorKha/homebrew-rkdeveloptool` first, then `brew install rkdeveloptool` |

### ADB / connection

| Symptom | Action |
|---|---|
| `adb devices` shows no device after boot | Wait 20 s and retry; unplug/replug USB; try a different port |
| `adb devices` shows device as "unauthorized" | Run `adb kill-server && adb start-server`, then retry |
| `udhcpc` loops with no IP | `wpa_supplicant` not associated — run `wpa_cli reassociate`, wait 3 s, retry |
| WiFi does not associate | Confirm SSID is 2.4 GHz — 5 GHz is unreliable on this firmware |
| SSH refused after reboot | WiFi not yet up — wait 10 s and retry |

### Installation

| Symptom | Action |
|---|---|
| `luckfox-install.sh` fails: "no luckfox release asset found" | No GitHub Release exists yet — push a `v*` tag to trigger CI, or use `--local` with a local build |
| `luckfox-install.sh` fails: "could not determine GitHub repository" | Check `git remote get-url origin` returns a valid GitHub URL |
| `luckfox-install.sh --local` fails: "local build incomplete" | Run `bash scripts/build-luckfox-mac.sh` first |

### Build

| Symptom | Action |
|---|---|
| Docker build fails: "no space left on device" | Docker Desktop → Settings → Resources → Disk; or `docker system prune -a` |
| Binary crashes on device: "not found" linker errors | uClibc mismatch — check with `ldd /opt/trap/yolo_v4l2` on the device |

### CLion

| Symptom | Action |
|---|---|
| "Compilation database not found" | Run `bash scripts/build-luckfox-mac.sh && bash scripts/clion-compile-commands.sh` |
| Headers unresolved after moving the repo | Sysroot path is absolute — re-run `clion-compile-commands.sh` then reload in CLion |

### Runtime

| Symptom | Action |
|---|---|
| `/dev/video0` not found | V4L2 driver not loaded — check `dmesg \| grep -i v4l` |
| Model load error in logs | Files missing — check `/opt/trap/model.ncnn.param` and `.bin` exist |
| WiFi AP not visible | `S50wifi` not running — check `ps \| grep hostapd` |
| Service won't start | `tail /var/log/ai-trap.log` and check `/opt/trap/trap_config.toml` |
