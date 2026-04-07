#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-esp32p4-mac.sh
#
#  Builds the ai-trap ESP32-P4 firmware using a locally-installed ESP-IDF.
#
#  Prerequisites (one-time setup)
#  ──────────────────────────────
#  1. Install dependencies:
#       brew install cmake ninja dfu-util python3
#
#  2. Clone ESP-IDF v5.3:
#       mkdir -p ~/esp && cd ~/esp
#       git clone --branch v5.3 --depth 1 https://github.com/espressif/esp-idf.git
#
#  3. Install the toolchain (RISC-V + ESP32-P4 support):
#       ~/esp/esp-idf/install.sh esp32p4
#
#  IDF location
#  ────────────
#  The script looks for ESP-IDF in this order:
#    1. $IDF_PATH environment variable (if already exported in your shell)
#    2. ~/esp/esp-idf  (default install location from Espressif docs)
#
#  Usage
#  ─────
#    bash scripts/build-esp32p4-mac.sh
#
#  Output
#  ──────
#    firmware-esp32p4/build/ai-trap-esp32p4.bin          (application)
#    firmware-esp32p4/build/bootloader/bootloader.bin
#    firmware-esp32p4/build/partition_table/partition-table.bin
#    firmware-esp32p4/build/compile_commands.json         (for CLion)
#
#  CLion integration
#  ─────────────────
#  After this script completes, run:
#    bash scripts/clion-esp32p4-compile-commands.sh
#  Then open build-esp32p4-clion/ in CLion (File → Open).
#
#  Subsequent builds
#  ─────────────────
#  managed_components/ is fetched on first build and reused automatically.
#  For a clean build:  rm -rf firmware-esp32p4/build && bash scripts/build-esp32p4-mac.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="${REPO_ROOT}/firmware-esp32p4"

# ── Locate ESP-IDF ────────────────────────────────────────────────────────────

if [[ -z "${IDF_PATH:-}" ]]; then
    if [[ -d "${HOME}/esp/esp-idf" ]]; then
        IDF_PATH="${HOME}/esp/esp-idf"
    else
        echo "ERROR: ESP-IDF not found." >&2
        echo >&2
        echo "  Install it:" >&2
        echo "    mkdir -p ~/esp && cd ~/esp" >&2
        echo "    git clone --branch v5.3 --depth 1 https://github.com/espressif/esp-idf.git" >&2
        echo "    ~/esp/esp-idf/install.sh esp32p4" >&2
        echo >&2
        echo "  Or set IDF_PATH to your existing install:" >&2
        echo "    export IDF_PATH=/path/to/esp-idf" >&2
        exit 1
    fi
fi

if [[ ! -f "${IDF_PATH}/export.sh" ]]; then
    echo "ERROR: ${IDF_PATH}/export.sh not found — IDF_PATH may be wrong." >&2
    exit 1
fi

echo "==> ESP-IDF : ${IDF_PATH}"
echo "    Project : ${PROJECT_DIR}"
echo

# ── Source IDF environment ────────────────────────────────────────────────────
#
# export.sh sets IDF_PATH, IDF_TOOLS_PATH, and prepends the RISC-V toolchain
# and idf.py to PATH.  We source it in a subshell context via 'env -i' so
# it doesn't pollute this script's environment with anything unexpected.
# Simpler: just source it directly — idf.py is what we need.

# shellcheck source=/dev/null
source "${IDF_PATH}/export.sh"

# ── Build ─────────────────────────────────────────────────────────────────────

cd "${PROJECT_DIR}"

# set-target writes sdkconfig from sdkconfig.defaults; safe to re-run
idf.py set-target esp32p4

# fetch managed components (skipped if managed_components/ already present)
idf.py update-dependencies

# full build — emits build/compile_commands.json alongside the binaries
idf.py build

echo
echo "Build complete."
echo "  Application : firmware-esp32p4/build/ai-trap-esp32p4.bin"
echo "  Bootloader  : firmware-esp32p4/build/bootloader/bootloader.bin"
echo "  Partitions  : firmware-esp32p4/build/partition_table/partition-table.bin"
echo
echo "CLion setup (run once after each clean build):"
echo "  bash scripts/clion-esp32p4-compile-commands.sh"
echo
echo "Flash (with ESP32-P4 connected via USB):"
echo "  idf.py -p /dev/cu.usbserial-* -b 2000000 flash"
