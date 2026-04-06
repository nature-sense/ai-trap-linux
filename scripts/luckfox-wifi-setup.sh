#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  luckfox-wifi-setup.sh
#
#  Stage 2 — Configure WiFi on the Luckfox Pico Zero via ADB.
#
#  Run this after luckfox-flash.sh, once the board has booted (ADB visible).
#
#  Usage
#  ─────
#    bash scripts/luckfox-wifi-setup.sh <SSID> <password>
#    bash scripts/luckfox-wifi-setup.sh               # prompts for SSID/password
#
#  Prerequisites
#  ─────────────
#    brew install android-platform-tools
#
#  The script will:
#    1. Wait for ADB device to appear
#    2. Write WiFi credentials to /etc/wpa_supplicant.conf
#    3. Connect wpa_supplicant and obtain an IP
#    4. Make WiFi start automatically on boot (/etc/network/interfaces)
#    5. Print the SSH command to use from now on
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${GREEN}[wifi-setup]${NC} $*"; }
warn()  { echo -e "${YELLOW}[wifi-setup]${NC} $*"; }
error() { echo -e "${RED}[wifi-setup]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${BOLD}── $* ──${NC}"; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

ensure_homebrew() {
    if command -v brew &>/dev/null; then
        info "Homebrew $(brew --version | head -1) ✓"
        return
    fi

    info "Homebrew not found — installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

    # Add Homebrew to PATH for the current session
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [[ -x /usr/local/bin/brew ]]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi

    command -v brew &>/dev/null || error "Homebrew installation failed."
    info "Homebrew installed ✓"
}

ensure_adb() {
    ensure_homebrew

    if ! command -v adb &>/dev/null; then
        info "adb not found — installing android-platform-tools..."
        brew install android-platform-tools
        info "adb installed ✓"
        return
    fi

    info "adb $(adb version | head -1) ✓"

    # Check for available update
    if brew outdated --formula 2>/dev/null | grep -q "^android-platform-tools"; then
        warn "android-platform-tools update available."
        read -r -p "  Upgrade now? [Y/n] " yn
        if [[ ! "${yn}" =~ ^[Nn]$ ]]; then
            brew upgrade android-platform-tools
            info "adb upgraded ✓"
        fi
    fi
}

step "Checking prerequisites"
ensure_adb

# ── Get credentials ───────────────────────────────────────────────────────────

SSID="${1:-}"
PASSWORD="${2:-}"

if [[ -z "${SSID}" ]]; then
    echo
    read -r -p "WiFi SSID     : " SSID
fi
if [[ -z "${PASSWORD}" ]]; then
    read -r -s -p "WiFi password : " PASSWORD
    echo
fi

[[ -n "${SSID}" ]]     || error "SSID cannot be empty."
[[ -n "${PASSWORD}" ]] || error "Password cannot be empty."

# ── Wait for ADB device ───────────────────────────────────────────────────────

step "Waiting for ADB device"

TIMEOUT=60
ELAPSED=0
while true; do
    if adb devices 2>/dev/null | grep -q "device$"; then
        break
    fi
    if [[ ${ELAPSED} -ge ${TIMEOUT} ]]; then
        error "ADB device not found after ${TIMEOUT}s.
       Check the board has booted (wait ~20 s after flash) and USB is connected."
    fi
    printf "\r  Waiting... %ds" "${ELAPSED}"
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done
echo
info "ADB device detected ✓"
adb devices

# ── Write WiFi credentials ────────────────────────────────────────────────────

step "Writing WiFi credentials"

# Use adb shell to edit wpa_supplicant.conf in-place
adb shell "sed -i 's/ssid=\".*\"/ssid=\"${SSID}\"/' /etc/wpa_supplicant.conf"
adb shell "sed -i 's/psk=\".*\"/psk=\"${PASSWORD}\"/' /etc/wpa_supplicant.conf"

# Verify
info "Credentials written. Verifying..."
adb shell "grep -E 'ssid|psk' /etc/wpa_supplicant.conf"

# ── Start wpa_supplicant ──────────────────────────────────────────────────────

step "Starting WiFi"

adb shell "
    killall wpa_supplicant 2>/dev/null || true
    rm -f /var/run/wpa_supplicant/wlan0
    wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
    sleep 5
    wpa_cli status
"

# Check association
WPA_STATE="$(adb shell "wpa_cli status 2>/dev/null | grep wpa_state" | tr -d '\r')"
info "  ${WPA_STATE}"

if ! echo "${WPA_STATE}" | grep -q "COMPLETED"; then
    warn "Not yet associated. Trying reassociate..."
    adb shell "wpa_cli reassociate && sleep 4"
    WPA_STATE="$(adb shell "wpa_cli status 2>/dev/null | grep wpa_state" | tr -d '\r')"
    info "  ${WPA_STATE}"
    if ! echo "${WPA_STATE}" | grep -q "COMPLETED"; then
        warn "Association did not complete. Check SSID/password and try again."
        warn "Note: Use 2.4 GHz SSID — 5 GHz may be unreliable on this firmware."
        exit 1
    fi
fi

info "Associated ✓"

# ── Get IP address ────────────────────────────────────────────────────────────

step "Obtaining IP address (udhcpc)"

adb shell "udhcpc -i wlan0 -q" || true
sleep 2

BOARD_IP="$(adb shell "ip addr show wlan0 2>/dev/null | grep 'inet '" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1 | tr -d '\r')"

if [[ -z "${BOARD_IP}" ]]; then
    warn "Could not determine IP address. Check your router's DHCP table."
else
    info "IP address: ${BOARD_IP}"
fi

# ── Make WiFi persist on boot ─────────────────────────────────────────────────

step "Making WiFi start on boot"

# Check if already configured
ALREADY="$(adb shell "grep -c 'wpa_supplicant' /etc/network/interfaces 2>/dev/null || echo 0" | tr -d '\r')"

if [[ "${ALREADY}" -gt 0 ]]; then
    info "WiFi boot config already present — skipping."
else
    adb shell "cat >> /etc/network/interfaces << 'EOF'

auto wlan0
iface wlan0 inet dhcp
    pre-up wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
    post-down killall wpa_supplicant
EOF"
    info "Boot config written ✓"
fi

# ── Done ──────────────────────────────────────────────────────────────────────

echo
echo "─────────────────────────────────────────────────────"
info "WiFi setup complete."
echo
if [[ -n "${BOARD_IP}" ]]; then
    echo "  SSH access:"
    echo "    ssh root@${BOARD_IP}"
    echo
    echo "  Verify SSH (password is blank or 'luckfox'):"
    echo "    ssh root@${BOARD_IP} 'uname -a && ls /dev/video*'"
fi
echo
echo "The USB cable is no longer needed for access."
echo "WiFi will start automatically on every boot."
echo "─────────────────────────────────────────────────────"
