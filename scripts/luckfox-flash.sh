#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  luckfox-flash.sh
#
#  Stage 1 — Flash the Luckfox Pico Zero EMMC firmware via rkdeveloptool.
#
#  The board must be in Maskrom mode before running this script:
#    1. Unplug USB
#    2. Hold the BOOT button
#    3. Plug USB-C into the board and into your Mac
#    4. Hold BOOT for 1–2 s then release
#
#  Usage
#  ─────
#    bash scripts/luckfox-flash.sh                     # use firmware in luckfox/system/
#    bash scripts/luckfox-flash.sh <firmware-dir>      # use a specific firmware directory
#
#  Prerequisites
#  ─────────────
#    brew tap IgorKha/homebrew-rkdeveloptool
#    brew install rkdeveloptool
#
#  After flashing, run Stage 2 to configure WiFi:
#    bash scripts/luckfox-wifi-setup.sh <SSID> <password>
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSTEM_DIR="${REPO_ROOT}/luckfox/system"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${GREEN}[flash]${NC} $*"; }
warn()  { echo -e "${YELLOW}[flash]${NC} $*"; }
error() { echo -e "${RED}[flash]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${BOLD}── $* ──${NC}"; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

ensure_homebrew() {
    if command -v brew &>/dev/null; then
        info "Homebrew $(brew --version | head -1) ✓"
        return
    fi

    info "Homebrew not found — installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

    # Add Homebrew to PATH for the current session (Apple Silicon path)
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [[ -x /usr/local/bin/brew ]]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi

    command -v brew &>/dev/null || error "Homebrew installation failed."
    info "Homebrew installed ✓"
}

ensure_rkdeveloptool() {
    ensure_homebrew

    if ! command -v rkdeveloptool &>/dev/null; then
        info "rkdeveloptool not found — installing..."
        brew tap IgorKha/homebrew-rkdeveloptool
        brew install rkdeveloptool
        info "rkdeveloptool installed ✓"
        return
    fi

    info "rkdeveloptool $(rkdeveloptool -v 2>&1 | head -1) ✓"

    # Check for available update (uses locally cached formula info — fast)
    if brew outdated --formula 2>/dev/null | grep -q "^rkdeveloptool"; then
        warn "rkdeveloptool update available."
        read -r -p "  Upgrade now? [Y/n] " yn
        if [[ ! "${yn}" =~ ^[Nn]$ ]]; then
            brew upgrade rkdeveloptool
            info "rkdeveloptool upgraded ✓"
        fi
    fi
}

step "Checking prerequisites"
ensure_rkdeveloptool

# ── Find firmware directory ───────────────────────────────────────────────────

if [[ -n "${1:-}" ]]; then
    FIRMWARE_DIR="$(cd "${1}" && pwd)"
    info "Using firmware: ${FIRMWARE_DIR}"
else
    # Auto-detect latest in luckfox/system/
    FIRMWARE_DIR="$(find "${SYSTEM_DIR}" -maxdepth 1 -type d -name 'Luckfox_Pico_Zero_EMMC_*' \
        | sort | tail -1)"

    if [[ -z "${FIRMWARE_DIR}" ]]; then
        error "No firmware found in ${SYSTEM_DIR}
       Import one first:
         bash scripts/luckfox-firmware-fetch.sh --import <path-to-downloaded-firmware>"
    fi
    info "Using firmware: $(basename "${FIRMWARE_DIR}")"
fi

# ── Validate firmware files ───────────────────────────────────────────────────

REQUIRED=(download.bin env.img idblock.img uboot.img boot.img oem.img userdata.img rootfs.img)
for f in "${REQUIRED[@]}"; do
    [[ -f "${FIRMWARE_DIR}/${f}" ]] || error "Missing required file: ${FIRMWARE_DIR}/${f}"
done
info "All required firmware files present ✓"

# ── Check board is in Maskrom mode ────────────────────────────────────────────

step "Checking for board in Maskrom mode"

DEVICES="$(sudo rkdeveloptool ld 2>/dev/null || true)"
if ! echo "${DEVICES}" | grep -q "0x110c"; then
    warn "Board not detected in Maskrom mode."
    echo
    echo "Put the board into Maskrom mode:"
    echo "  1. Unplug USB cable"
    echo "  2. Hold the BOOT button"
    echo "  3. Plug USB-C into the board and your Mac while holding BOOT"
    echo "  4. Hold for 1–2 s then release"
    echo
    read -r -p "Press Enter when board is in Maskrom mode, or Ctrl-C to abort..."
    DEVICES="$(sudo rkdeveloptool ld 2>/dev/null || true)"
    echo "${DEVICES}" | grep -q "0x110c" || error "Board still not detected in Maskrom mode."
fi

info "Board detected in Maskrom mode ✓"
echo "${DEVICES}" | grep "0x110c"

# ── Flash ─────────────────────────────────────────────────────────────────────

cd "${FIRMWARE_DIR}"

step "Loading DDR initialisation (download.bin)"
sudo rkdeveloptool db download.bin
info "Bootloader loaded ✓"

# Partition layout from sd_update.txt:
#   env       0x00000   uboot env / GPT info
#   idblock   0x00040   idbloader (DDR init + miniloader)
#   uboot     0x00440   U-Boot proper
#   boot      0x00640   kernel + DTB
#   oem       0x10640   OEM data partition
#   userdata  0x110640  userdata partition
#   rootfs    0x190640  root filesystem

step "Writing partitions"
flash_part() {
    local sector="$1" file="$2"
    info "  wl ${sector}  ${file}  ($(du -sh "${file}" | cut -f1))"
    sudo rkdeveloptool wl "${sector}" "${file}"
}

flash_part 0x0      env.img
flash_part 0x40     idblock.img
flash_part 0x440    uboot.img
flash_part 0x640    boot.img
flash_part 0x10640  oem.img
flash_part 0x110640 userdata.img
flash_part 0x190640 rootfs.img

step "Rebooting board"
sudo rkdeveloptool rd
info "Board rebooting..."

echo
echo "─────────────────────────────────────────────────────"
info "Flash complete."
echo
echo "Wait ~20 s for the board to boot, then run Stage 2:"
echo "  bash scripts/luckfox-wifi-setup.sh <SSID> <password>"
echo "─────────────────────────────────────────────────────"
