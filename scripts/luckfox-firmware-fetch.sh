#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  luckfox-firmware-fetch.sh
#
#  Manages the Luckfox Pico Zero EMMC firmware stored in luckfox/system/.
#
#  Usage
#  ─────
#    bash scripts/luckfox-firmware-fetch.sh              # show current status
#    bash scripts/luckfox-firmware-fetch.sh --import <dir>  # import a downloaded firmware dir
#
#  The Luckfox firmware is distributed via Google Drive and cannot be
#  downloaded automatically.  This script manages what is stored locally
#  and tells you where to get a newer version.
#
#  Download URL:
#    https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads/
#    → Google Drive → Luckfox_Pico_Zero_EMMC_YYMMDD folder
#
#  After downloading, import into the project:
#    bash scripts/luckfox-firmware-fetch.sh --import ~/Downloads/Luckfox_Pico_Zero_EMMC_250802
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSTEM_DIR="${REPO_ROOT}/luckfox/system"

REQUIRED_FILES=(
    "download.bin"
    "idblock.img"
    "env.img"
    "uboot.img"
    "boot.img"
    "oem.img"
    "userdata.img"
    "rootfs.img"
    "update.img"
)

DOWNLOAD_URL="https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads/"

# ── Helpers ───────────────────────────────────────────────────────────────────

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${GREEN}[firmware]${NC} $*"; }
warn()  { echo -e "${YELLOW}[firmware]${NC} $*"; }
error() { echo -e "${RED}[firmware]${NC} $*" >&2; }

firmware_version() {
    local dir="$1"
    basename "${dir}" | grep -oE '[0-9]{6}$' || echo "unknown"
}

validate_firmware() {
    local dir="$1"
    local missing=()
    for f in "${REQUIRED_FILES[@]}"; do
        [[ -f "${dir}/${f}" ]] || missing+=("${f}")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "Missing files in ${dir}:"
        for f in "${missing[@]}"; do
            warn "  ✗ ${f}"
        done
        return 1
    fi
    return 0
}

# ── Find current firmware ─────────────────────────────────────────────────────

find_current() {
    # Return the most recent firmware directory (sorted by date suffix)
    find "${SYSTEM_DIR}" -maxdepth 1 -type d -name 'Luckfox_Pico_Zero_EMMC_*' \
        | sort | tail -1
}

# ── Status ────────────────────────────────────────────────────────────────────

show_status() {
    echo
    echo -e "${BOLD}Luckfox Pico Zero — firmware status${NC}"
    echo "Storage : ${SYSTEM_DIR}"
    echo

    local current
    current="$(find_current)"

    if [[ -z "${current}" ]]; then
        warn "No firmware found in luckfox/system/"
        echo
        echo "Download the latest from:"
        echo "  ${DOWNLOAD_URL}"
        echo "  → Google Drive → Luckfox_Pico_Zero_EMMC_YYMMDD folder"
        echo
        echo "Then import it:"
        echo "  bash scripts/luckfox-firmware-fetch.sh --import ~/Downloads/Luckfox_Pico_Zero_EMMC_YYMMDD"
        echo
        return
    fi

    local ver
    ver="$(firmware_version "${current}")"
    info "Current firmware : ${ver}  ($(basename "${current}"))"

    if validate_firmware "${current}"; then
        info "All required files present ✓"
    fi

    # List all stored versions if more than one
    local count
    count="$(find "${SYSTEM_DIR}" -maxdepth 1 -type d -name 'Luckfox_Pico_Zero_EMMC_*' | wc -l | tr -d ' ')"
    if [[ "${count}" -gt 1 ]]; then
        echo
        echo "All stored versions:"
        find "${SYSTEM_DIR}" -maxdepth 1 -type d -name 'Luckfox_Pico_Zero_EMMC_*' | sort | while read -r d; do
            local marker=""
            [[ "${d}" == "${current}" ]] && marker=" ← current"
            echo "  $(basename "${d}")${marker}"
        done
    fi

    echo
    echo "To check for a newer release, visit:"
    echo "  ${DOWNLOAD_URL}"
    echo
    echo "To import a newer download:"
    echo "  bash scripts/luckfox-firmware-fetch.sh --import ~/Downloads/Luckfox_Pico_Zero_EMMC_YYMMDD"
}

# ── Import ────────────────────────────────────────────────────────────────────

do_import() {
    local src="${1:?Usage: --import <path-to-firmware-dir>}"
    src="$(cd "${src}" && pwd)"  # resolve absolute path

    local dirname
    dirname="$(basename "${src}")"

    # Accept any directory containing the required files, not just dated ones
    if ! [[ "${dirname}" =~ ^Luckfox_Pico_Zero_EMMC_ ]]; then
        warn "Directory name '${dirname}' does not look like a Luckfox firmware package."
        warn "Expected: Luckfox_Pico_Zero_EMMC_YYMMDD"
        read -r -p "Import anyway? [y/N] " yn
        [[ "${yn}" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 0; }
    fi

    local dst="${SYSTEM_DIR}/${dirname}"

    if [[ -d "${dst}" ]]; then
        info "Firmware ${dirname} already exists at ${dst}"
        info "Validating..."
        validate_firmware "${dst}" && info "OK — nothing to do." || true
        exit 0
    fi

    info "Validating source: ${src}"
    if ! validate_firmware "${src}"; then
        error "Source directory is missing required files. Aborting."
        exit 1
    fi

    info "Importing → ${dst}"
    mkdir -p "${SYSTEM_DIR}"
    cp -r "${src}" "${dst}"

    info "Import complete."
    info "Firmware ${dirname} is ready."
    echo
    echo "Flash to board:"
    echo "  bash scripts/luckfox-flash.sh"
}

# ── Main ──────────────────────────────────────────────────────────────────────

ensure_homebrew() {
    if command -v brew &>/dev/null; then
        return
    fi
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

mkdir -p "${SYSTEM_DIR}"

case "${1:-}" in
    --import|-i)
        do_import "${2:-}"
        ;;
    --help|-h)
        echo "Usage:"
        echo "  bash scripts/luckfox-firmware-fetch.sh                  # show status"
        echo "  bash scripts/luckfox-firmware-fetch.sh --import <dir>   # import firmware"
        ;;
    "")
        show_status
        ;;
    *)
        error "Unknown argument: ${1}"
        exit 1
        ;;
esac
