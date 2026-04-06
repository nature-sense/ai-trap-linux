#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  luckfox-setup.sh
#
#  End-to-end setup of a brand-new Luckfox Pico Zero insect trap.
#  Runs all four stages in sequence:
#
#    Stage 1 — Flash OS firmware via rkdeveloptool
#    Stage 2 — Install ai-trap software via ADB
#    Stage 3 — Configure WiFi via ADB
#    Stage 4 — Reboot into production mode
#
#  Usage
#  ─────
#    bash scripts/luckfox-setup.sh
#    bash scripts/luckfox-setup.sh "MyNetwork" "MyPassword"
#
#  What you need
#  ─────────────
#    • USB-C cable (data-capable)
#    • The Luckfox firmware in luckfox/system/ — if missing, run:
#        bash scripts/luckfox-firmware-fetch.sh --import <firmware-dir>
#    • Internet connection (to download tools and/or GitHub Release)
#
#  The script prompts for WiFi credentials at the start so it can run
#  unattended after the board enters Maskrom mode.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS="${REPO_ROOT}/scripts"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${GREEN}[setup]${NC} $*"; }
warn()    { echo -e "${YELLOW}[setup]${NC} $*"; }
error()   { echo -e "${RED}[setup]${NC} $*" >&2; exit 1; }
stage()   { echo -e "\n${CYAN}${BOLD}━━━  $*  ━━━${NC}\n"; }
divider() { echo -e "${BOLD}─────────────────────────────────────────────────────${NC}"; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

ensure_homebrew() {
    if command -v brew &>/dev/null; then return; fi
    info "Homebrew not found — installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
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
    fi
    if brew outdated --formula 2>/dev/null | grep -q "^android-platform-tools"; then
        warn "android-platform-tools update available."
        read -r -p "  Upgrade now? [Y/n] " yn
        [[ "${yn}" =~ ^[Nn]$ ]] || brew upgrade android-platform-tools
    fi
    info "adb $(adb version | head -1) ✓"
}

# ── Check prerequisites before doing anything else ───────────────────────────

ensure_adb

# ── Banner ────────────────────────────────────────────────────────────────────

clear
divider
echo -e "${BOLD}  ai-trap Luckfox Pico Zero — Full Setup${NC}"
divider
echo
echo "This script will:"
echo "  1. Flash the Luckfox OS firmware"
echo "  2. Install ai-trap software"
echo "  3. Configure WiFi"
echo "  4. Reboot into production mode"
echo

# ── Collect WiFi credentials up front ────────────────────────────────────────

WIFI_SSID="${1:-}"
WIFI_PASS="${2:-}"

if [[ -z "${WIFI_SSID}" ]]; then
    read -r -p "WiFi SSID     : " WIFI_SSID
fi
if [[ -z "${WIFI_PASS}" ]]; then
    read -r -s -p "WiFi password : " WIFI_PASS
    echo
fi

[[ -n "${WIFI_SSID}" ]] || error "SSID cannot be empty."
[[ -n "${WIFI_PASS}" ]] || error "Password cannot be empty."

echo
info "WiFi credentials saved. Starting setup..."
echo
echo "  SSID : ${WIFI_SSID}"
echo "  Note : Use 2.4 GHz SSID — 5 GHz may be unreliable on this firmware."
echo

# ── Check firmware is available ───────────────────────────────────────────────

SYSTEM_DIR="${REPO_ROOT}/luckfox/system"
FIRMWARE="$(find "${SYSTEM_DIR}" -maxdepth 1 -type d -name 'Luckfox_Pico_Zero_EMMC_*' \
    | sort | tail -1)"

if [[ -z "${FIRMWARE}" ]]; then
    error "No firmware found in luckfox/system/.
       Import it first:
         bash scripts/luckfox-firmware-fetch.sh --import <path-to-firmware>"
fi
info "Firmware : $(basename "${FIRMWARE}")"

# ── Stage 1: Flash ────────────────────────────────────────────────────────────

stage "Stage 1 of 4 — Flash OS firmware"

echo "Put the board into Maskrom mode:"
echo "  1. Unplug the USB cable from the board"
echo "  2. Hold the BOOT button on the board"
echo "  3. While holding BOOT, plug the USB-C cable in"
echo "  4. Hold for 1–2 seconds, then release BOOT"
echo
read -r -p "Press Enter when the board is in Maskrom mode..."

bash "${SCRIPTS}/luckfox-flash.sh"

# ── Wait for board to boot ────────────────────────────────────────────────────

stage "Waiting for board to boot"

info "Board is rebooting after flash. Waiting for ADB..."
echo
TIMEOUT=90
ELAPSED=0
while ! adb devices 2>/dev/null | grep -q "device$"; do
    if [[ ${ELAPSED} -ge ${TIMEOUT} ]]; then
        error "Board did not appear as an ADB device after ${TIMEOUT}s.
       Try unplugging and replugging the USB cable, then re-run this script."
    fi
    printf "\r  Waiting... %ds" "${ELAPSED}"
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done
echo
info "Board booted and ADB connected ✓"

# ── Stage 2: Install software ─────────────────────────────────────────────────

stage "Stage 2 of 4 — Install ai-trap software"

bash "${SCRIPTS}/luckfox-install.sh"

# ── Stage 3: Configure WiFi ───────────────────────────────────────────────────

stage "Stage 3 of 4 — Configure WiFi"

bash "${SCRIPTS}/luckfox-wifi-setup.sh" "${WIFI_SSID}" "${WIFI_PASS}"

# ── Stage 4: Final reboot ─────────────────────────────────────────────────────

stage "Stage 4 of 4 — Final reboot"

info "Rebooting board into production mode..."
adb shell "reboot" 2>/dev/null || true

echo
info "Waiting for board to come up on WiFi (~30 s)..."
sleep 30

# ── Done ──────────────────────────────────────────────────────────────────────

echo
divider
echo -e "${BOLD}  Setup complete!${NC}"
divider
echo
echo "The Luckfox is now running as an insect trap."
echo
echo "Connect to it over WiFi:"
echo "  ssh root@<ip-shown-above>"
echo
echo "Check the service is running:"
echo "  ssh root@<ip> '/etc/init.d/S99trap status'"
echo "  ssh root@<ip> 'tail -f /var/log/ai-trap.log'"
echo
echo "Configure the trap (ID, location, thresholds) via the REST API:"
echo "  curl http://<ip>:8080/api/config"
echo "  curl -X POST http://<ip>:8080/api/config -H 'Content-Type: application/json' \\"
echo "       -d '{\"trap\":{\"id\":\"trap_001\",\"location\":\"Garden\"}}'"
echo
echo "The USB cable is no longer needed."
divider
