# Luckfox Pico Zero — Installation Guide (Mac)

## Overview

The Luckfox Pico Zero runs a minimal Linux (busybox/uClibc) on an RV1106G3
(Cortex-A7) SoC. The cross-compilation toolchain is Linux-only and cannot run
natively on macOS. The easiest approach is to download a pre-built binary from
GitHub Actions, then deploy it to the board over USB or WiFi.

---

## 0. Flash the OS image (first-time setup)

Skip this section if the board already has a working OS image.

### 0.1 Download the OS image and flashing tool

Go to the Luckfox downloads page:
**https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads/**

From the Google Drive folder, download:
- The latest `Luckfox_Pico_Zero_EMMC_YYMMDD.img` image file
- The `upgrade_tool_v2.44_mac` folder — pick the binary matching your macOS version

### 0.2 Unblock the upgrade_tool binary

`upgrade_tool` is unsigned and macOS Gatekeeper will block it. Run these once before first use:

```bash
cd upgrade_tool_v2.44_mac
xattr -d com.apple.quarantine ./upgrade_tool
codesign -s - ./upgrade_tool
```

If a blocked-app dialog still appears: **System Settings → Privacy & Security → Open Anyway**.

> **Critical: use a data-capable USB-C cable.** Charge-only cables have no data lines and the
> board will be completely invisible to the Mac — nothing appears in System Profiler, no LED
> in Maskrom mode, no error message. If the board doesn't appear, try a different cable first.
> A cable that works for file transfer with a phone or USB drive is suitable.

### 0.3 Put the board into Maskrom mode

1. With the board **unpowered** and USB cable **disconnected**, hold down the **BOOT** button
2. While holding BOOT, plug the USB-C cable into the board and connect to your Mac
3. Hold BOOT for 1–2 seconds after connecting, then release

Verify the board is detected:

```bash
system_profiler SPUSBDataType | grep -A5 "Rockchip\|2207"
```

A device with Vendor ID `0x2207` should appear. If not, try a different USB port —
USB 3.x ports on some Macs cause detection failures; a USB 2.0 port or hub is more reliable.

### 0.4 Flash the image

```bash
sudo ./upgrade_tool uf /path/to/Luckfox_Pico_Zero_EMMC_YYMMDD.img
```

Expected output on success:
```
Download Boot Success
Download IDB Success
Download Firmware Success
Upgrade firmware ok.
```

The board reboots automatically after flashing.

### 0.5 Verify boot

After flashing, the board presents a USB network interface. SSH in:

```bash
ssh root@172.32.0.93
```

Default credentials: `root` with no password (blank). If you get a shell prompt, the flash succeeded.

> **Fallback — rkdeveloptool (if upgrade_tool fails):**
> ```bash
> brew tap IgorKha/homebrew-rkdeveloptool
> brew install rkdeveloptool
> sudo rkdeveloptool ld                           # confirm MaskRom detected
> sudo rkdeveloptool db rv1106_loader.bin          # loader .bin from same Google Drive folder
> sudo rkdeveloptool wl 0 Luckfox_Pico_Zero_EMMC_YYMMDD.img
> ```

> **Fallback — browser WebUSB tool (no install needed):**
> Open **https://asadmemon.com/rkdeveloptool/** in Chrome with the board in Maskrom mode.
> Tested on RV1106 Luckfox boards.

---

## 1. Get the binary

### Option A — Download from GitHub Actions (recommended)

1. Go to the repository on GitHub
2. Click **Actions** → **Build · Luckfox Pico Zero**
3. Open the latest successful run
4. Download the artifact: `yolo_v4l2-rv1106-<sha>`
5. Unzip — you get the stripped `yolo_v4l2` binary

### Option B — Build via Docker on Mac

```bash
# From the repository root
docker run --rm -v "$(pwd)":/work -w /work ubuntu:22.04 \
  bash -c "
    apt-get update -qq && \
    apt-get install -y --no-install-recommends \
      cmake ninja-build meson pkg-config curl ca-certificates git \
      libbluetooth-dev && \
    bash scripts/build-luckfox-deps.sh && \
    meson setup build-luckfox \
      --cross-file cross/luckfox-rv1106.ini \
      -Dtarget=v4l2 \
      -Dluckfox_sysroot=sysroot/luckfox && \
    ninja -C build-luckfox
  "
```

The binary is at `build-luckfox/yolo_v4l2`.

---

## 2. Connect to the Luckfox via USB

The Luckfox Pico Zero creates a USB Ethernet gadget (RNDIS/NCM) when plugged
into your Mac via USB-C.

1. Plug the Luckfox into your Mac with a USB-C cable
2. On your Mac: **System Settings → Network** — a new USB Ethernet interface
   appears. It may need a moment to configure.
3. SSH in:

```bash
ssh root@172.32.0.70
```

Default password: `luckfox` (or blank — check your firmware version).

> **If connection is refused:** The board may be running an older firmware. Try
> `172.32.0.93` or check `System Settings → Network` for the assigned IP on
> the USB interface, then scan: `arp -a | grep -i rockchip`

---

## 3. Deploy the package

From your Mac, copy the install package to the Luckfox:

```bash
# Create a staging directory with everything needed
mkdir -p /tmp/ai-trap-luckfox
cp path/to/yolo_v4l2                              /tmp/ai-trap-luckfox/
cp firmware/models/yolo11n-320/model.ncnn.param   /tmp/ai-trap-luckfox/
cp firmware/models/yolo11n-320/model.ncnn.bin     /tmp/ai-trap-luckfox/
cp package/luckfox/install.sh                     /tmp/ai-trap-luckfox/
cp package/luckfox/S50wifi                        /tmp/ai-trap-luckfox/
cp package/config/trap_config.toml                /tmp/ai-trap-luckfox/

# Copy to the board
scp -r /tmp/ai-trap-luckfox root@172.32.0.70:/tmp/

# Run the installer
ssh root@172.32.0.70 "sh /tmp/ai-trap-luckfox/install.sh"
```

---

## 4. Configure WiFi

On first boot (or after a clean install with no `wifi_creds.conf`), the Luckfox
starts in **AP mode**:

| Setting  | Value                       |
|----------|-----------------------------|
| SSID     | `ai-trap-luckfox_001`       |
| Password | `aiwildlife`                |
| IP       | `192.168.4.1`               |

Connect your Mac (or phone) to this WiFi network, then provision your router
credentials:

```bash
curl -X POST http://192.168.4.1:8080/api/wifi \
  -H "Content-Type: application/json" \
  -d '{"ssid":"YourNetwork","password":"YourPassword"}'
```

The Luckfox reboots into station mode and joins your network. Find its IP on
your router, or check via ARP once connected to the same network:

```bash
arp -a | grep -i rockchip
```

To revert to AP mode later:

```bash
curl -X POST http://<luckfox-ip>:8080/api/wifi/reset
```

---

## 5. Verify the software is running

```bash
ssh root@<luckfox-ip>
/etc/init.d/S99trap status      # or: ps | grep yolo_v4l2
tail -f /var/log/ai-trap.log
```

The camera should appear at `/dev/video0`. If not:

```bash
ls /dev/video*
v4l2-ctl --list-devices
```

---

## 6. Manual start / stop

```bash
/etc/init.d/S50wifi start       # start WiFi
/etc/init.d/S99trap start       # start ai-trap
/etc/init.d/S99trap stop
/etc/init.d/S99trap restart
```

Services start automatically on boot via `/etc/init.d/`.

---

## 7. Updating the binary

```bash
# Stop the service
ssh root@<luckfox-ip> "/etc/init.d/S99trap stop"

# Copy new binary
scp path/to/yolo_v4l2 root@<luckfox-ip>:/opt/trap/yolo_v4l2

# Restart
ssh root@<luckfox-ip> "/etc/init.d/S99trap start"
```

---

## USB networking on macOS — first-time setup

The Luckfox presents a **RNDIS** USB network gadget. macOS has no native RNDIS driver so no
USB network interface appears even though the board is detected. Use **ADB** to get initial
access, then configure WiFi so you can SSH over your normal network.

### Connect via ADB

```bash
brew install android-platform-tools
adb devices                  # shows something like "0c71d25c71813677  device"
adb shell                    # root shell, no password needed
```

### Configure WiFi from the ADB shell

```bash
# Check your router's SSID from scan results
wpa_cli scan && sleep 3 && wpa_cli scan_results

# Edit credentials (replace with your actual SSID and password)
sed -i 's/ssid=".*"/ssid="YourSSID"/' /etc/wpa_supplicant.conf
sed -i 's/psk=".*"/psk="YourPassword"/' /etc/wpa_supplicant.conf

# Start wpa_supplicant
killall wpa_supplicant 2>/dev/null
rm -f /var/run/wpa_supplicant/wlan0
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
sleep 5
wpa_cli status               # wpa_state should show COMPLETED

# If ARP doesn't resolve on first try, reassociate
wpa_cli reassociate && sleep 3

# Get an IP
udhcpc -i wlan0
ip addr show wlan0           # note the assigned IP
```

### Make WiFi start on boot

```bash
cat >> /etc/network/interfaces << 'EOF'

auto wlan0
iface wlan0 inet dhcp
    pre-up wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
    post-down killall wpa_supplicant
EOF
reboot
```

After reboot, SSH over WiFi using the IP shown above (or check your router's DHCP table).

> **Note:** The Luckfox Pico Zero's AIC8800DC supports 2.4 GHz and 5 GHz, but the 2.4 GHz
> band is more reliable with the stock firmware. If your router uses separate SSIDs per band,
> use the 2.4 GHz SSID.

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `ssh: connect to host 172.32.0.70 port 22: Connection refused` | Board not booted yet, or try `172.32.0.93` |
| Board invisible in Maskrom mode — no LED, nothing in System Profiler | Cable is charge-only — replace with a data-capable cable |
| No USB network interface on Mac after boot | RNDIS not supported natively — use ADB instead (see above) |
| `adb devices` shows no device | Board not booted, or try a different USB port/cable |
| `udhcpc` loops with no response | wpa_supplicant not associated — run `wpa_cli reassociate` then retry |
| ARP incomplete, ping fails | Run `wpa_cli reassociate`, wait 3s, retry ping |
| `/dev/video0` not found | Camera not detected — check V4L2 driver is loaded |
| `ai-trap.log` shows model load error | Model files missing — check `/opt/trap/model.ncnn.*` |
| WiFi AP not visible | `S50wifi` not running — check `ps | grep hostapd` |
