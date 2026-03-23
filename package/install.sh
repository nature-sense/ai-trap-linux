#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  install.sh — AI Trap installation script for Raspberry Pi 5 (aarch64)
#
#  Usage:
#    sudo ./install.sh
#
#  Installs to /opt/ai-trap and registers a systemd service.
#  An existing trap_config.toml is never overwritten.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="/opt/ai-trap"
SERVICE_NAME="ai-trap"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
BINARY_NAME="yolo_libcamera"
CONFIG_NAME="trap_config.toml"
SERVICE_USER="ai-trap"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[ai-trap]${NC} $*"; }
warn()  { echo -e "${YELLOW}[ai-trap]${NC} $*"; }
error() { echo -e "${RED}[ai-trap]${NC} $*" >&2; exit 1; }

# ── Pre-flight checks ─────────────────────────────────────────────────────────

[ "$(id -u)" -eq 0 ] || error "Please run as root: sudo ./install.sh"

ARCH=$(uname -m)
[ "$ARCH" = "aarch64" ] || error "This package is for aarch64 (Raspberry Pi 5). Detected: $ARCH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "${SCRIPT_DIR}/bin/${BINARY_NAME}" ]    || error "Binary not found: ${SCRIPT_DIR}/bin/${BINARY_NAME}"
[ -f "${SCRIPT_DIR}/systemd/${SERVICE_NAME}.service" ] || error "Service file not found"

# ── Create system user ────────────────────────────────────────────────────────

if ! id "${SERVICE_USER}" &>/dev/null; then
    info "Creating system user '${SERVICE_USER}'..."
    useradd --system --no-create-home --shell /usr/sbin/nologin \
            --groups video "${SERVICE_USER}"
else
    info "User '${SERVICE_USER}' already exists — skipping."
    # Ensure the user is in the video group for camera access
    usermod -aG video "${SERVICE_USER}" 2>/dev/null || true
fi

# ── Create directory structure ────────────────────────────────────────────────

info "Creating ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}/bin"
mkdir -p "${INSTALL_DIR}/lib"
mkdir -p "${INSTALL_DIR}/lib/ipa"
mkdir -p "${INSTALL_DIR}/models"
mkdir -p "${INSTALL_DIR}/crops"
mkdir -p "${INSTALL_DIR}/logs"
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}"
chmod 755 "${INSTALL_DIR}"

# ── Install binary ────────────────────────────────────────────────────────────

info "Installing binary..."
install -o root -g root -m 755 \
    "${SCRIPT_DIR}/bin/${BINARY_NAME}" \
    "${INSTALL_DIR}/bin/${BINARY_NAME}"

# ── Install bundled libcamera runtime (if present in package) ────────────────
# Allows deploying a specific libcamera version alongside the binary so the
# binary runs regardless of which libcamera version the OS has installed.
# The binary has RPATH=/opt/ai-trap/lib so it picks these up automatically.

if [ -d "${SCRIPT_DIR}/lib" ] && [ -n "$(ls -A "${SCRIPT_DIR}/lib" 2>/dev/null)" ]; then
    info "Installing bundled libraries..."
    cp -r "${SCRIPT_DIR}/lib/"* "${INSTALL_DIR}/lib/"
    # Proxy must be executable — libcamera spawns it as a subprocess.
    chmod +x "${INSTALL_DIR}/lib/proxy/"* 2>/dev/null || true
else
    info "No bundled libs found — using system libraries."
fi

# Camera tuning JSON files must live at the path the IPA module expects.
if [ -d "${SCRIPT_DIR}/share/libcamera" ]; then
    info "Installing camera tuning files..."
    mkdir -p /usr/share/libcamera/ipa/rpi/pisp
    cp -r "${SCRIPT_DIR}/share/libcamera/ipa/rpi/pisp/"*.json \
          /usr/share/libcamera/ipa/rpi/pisp/
fi

# IPA proxy executable must be in the libexec path libcamera expects.
if [ -f "${SCRIPT_DIR}/lib/proxy/raspberrypi_ipa_proxy" ]; then
    info "Installing IPA proxy executable..."
    mkdir -p /usr/libexec/aarch64-linux-gnu/libcamera
    install -m 755 "${SCRIPT_DIR}/lib/proxy/raspberrypi_ipa_proxy" \
        /usr/libexec/aarch64-linux-gnu/libcamera/raspberrypi_ipa_proxy
fi

# ── Install config (never overwrite an existing one) ─────────────────────────

if [ -f "${INSTALL_DIR}/${CONFIG_NAME}" ]; then
    warn "Config already exists at ${INSTALL_DIR}/${CONFIG_NAME} — not overwriting."
    warn "Reference config saved to ${INSTALL_DIR}/${CONFIG_NAME}.new"
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/config/${CONFIG_NAME}" \
        "${INSTALL_DIR}/${CONFIG_NAME}.new"
else
    info "Installing default config..."
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/config/${CONFIG_NAME}" \
        "${INSTALL_DIR}/${CONFIG_NAME}"

    # Set trap ID from hostname (e.g. trap004 → id = "trap004")
    TRAP_ID=$(hostname)
    if [ -n "${TRAP_ID}" ]; then
        sed -i "s/^id\s*=.*/id       = \"${TRAP_ID}\"/" "${INSTALL_DIR}/${CONFIG_NAME}"
        info "Trap ID set to '${TRAP_ID}' from hostname"
    fi
fi

# ── Seed WiFi credentials from existing NetworkManager config ────────────────
# If the Pi was flashed with WiFi pre-configured via Raspberry Pi Imager,
# copy those credentials to wifi_creds.conf so the WiFi manager (managed=true)
# uses them on first boot instead of starting an AP.

CREDS_FILE="${INSTALL_DIR}/wifi_creds.conf"
if [ ! -f "${CREDS_FILE}" ]; then
    NM_SSID=$(nmcli -t -f active,ssid dev wifi 2>/dev/null | grep '^yes' | cut -d: -f2 | head -1)
    if [ -n "${NM_SSID}" ]; then
        NM_PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show "${NM_SSID}" 2>/dev/null || true)
        if [ -n "${NM_PSK}" ]; then
            printf "ssid=%s\npassword=%s\n" "${NM_SSID}" "${NM_PSK}" > "${CREDS_FILE}"
            chown "${SERVICE_USER}:${SERVICE_USER}" "${CREDS_FILE}"
            chmod 600 "${CREDS_FILE}"
            info "Seeded wifi_creds.conf from NetworkManager (SSID: ${NM_SSID})"
        else
            info "WiFi connected but PSK not readable — wifi_creds.conf not seeded."
        fi
    else
        info "No active WiFi connection found — trap will start in AP mode on first boot."
    fi
fi

# ── Bluetooth daemon ──────────────────────────────────────────────────────────
# When wifi.managed = true, the trap binary drives BLE directly via raw HCI
# sockets; bluetoothd must not run in that mode (enable BT_DISABLE below).
# When wifi.managed = false (default), BLE is unused — leave bluetoothd alone.
# WARNING: disabling bluetooth.service on Pi 5 can prevent WiFi from
# initialising on next boot because BT and WiFi share the same firmware.
#
# BT_DISABLE=true  → uncomment to disable bluetoothd (managed=true deployments)
BT_DISABLE=false
if [ "${BT_DISABLE}" = "true" ]; then
    info "Disabling system Bluetooth daemon (bluetoothd)..."
    systemctl disable --now bluetooth.service 2>/dev/null || true
fi



# ── Install systemd service ───────────────────────────────────────────────────

info "Installing systemd services..."
install -o root -g root -m 644 \
    "${SCRIPT_DIR}/systemd/${SERVICE_NAME}.service" \
    "${SERVICE_FILE}"
install -o root -g root -m 644 \
    "${SCRIPT_DIR}/systemd/camera-overlay.service" \
    /etc/systemd/system/camera-overlay.service

systemctl daemon-reload
systemctl enable camera-overlay.service
systemctl enable "${SERVICE_NAME}.service"

# ── Enable persistent journal so logs survive reboot ─────────────────────────
mkdir -p /var/log/journal
systemd-tmpfiles --create --prefix /var/log/journal

# ── Install model files ───────────────────────────────────────────────────────

MODEL_DIR="${INSTALL_DIR}/models/yolo11n-320"
MODEL_PARAM="${MODEL_DIR}/model.ncnn.param"
MODEL_BIN="${MODEL_DIR}/model.ncnn.bin"

if [ -d "${SCRIPT_DIR}/models/yolo11n-320" ]; then
    info "Installing model files..."
    mkdir -p "${MODEL_DIR}"
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/models/yolo11n-320/model.ncnn.param" "${MODEL_PARAM}"
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/models/yolo11n-320/model.ncnn.bin"   "${MODEL_BIN}"
fi

if [ ! -f "${MODEL_PARAM}" ] || [ ! -f "${MODEL_BIN}" ]; then
    warn "Model files not found — service will not start on boot."
    warn "Copy model files to ${MODEL_DIR}/ then reboot or run:"
    warn "  sudo systemctl start ${SERVICE_NAME}"
fi

# ── Done ──────────────────────────────────────────────────────────────────────

info "Installation complete."
info ""
info "Useful commands:"
info "  sudo systemctl status  ${SERVICE_NAME}   — check status"
info "  sudo systemctl stop    ${SERVICE_NAME}   — stop"
info "  sudo systemctl start   ${SERVICE_NAME}   — start"
info "  sudo journalctl -u     ${SERVICE_NAME} -f  — follow logs"
info ""
info "Config:  ${INSTALL_DIR}/${CONFIG_NAME}"
info "Crops:   ${INSTALL_DIR}/crops/"
info "DB:      ${INSTALL_DIR}/detections.db"
info "API:     http://<pi-ip>:8080"
info "Stream:  http://<pi-ip>:9000"
info "BLE:     advertises as 'AI-Trap-<trap_id>' (requires wifi.managed=true)"
info ""
info "NOTE: camera-overlay.service loads dtoverlay=imx708 at runtime"
info "after the network is online, avoiding boot-time power spikes."
