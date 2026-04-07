#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  clion-esp32p4-compile-commands.sh
#
#  Copies compile_commands.json from the local ESP-IDF build into the CLion
#  project directory.
#
#  Because ESP-IDF is installed locally, all paths in compile_commands.json
#  are already real Mac filesystem paths — no remapping is needed.
#
#  Run this after build-esp32p4-mac.sh:
#    bash scripts/clion-esp32p4-compile-commands.sh
#
#  Then in CLion:
#    File → Open → build-esp32p4-clion/
#    (select "Open as Project" if prompted)
#
#  CLion will index:
#    • firmware-esp32p4/main/          — your source files (fully resolved)
#    • firmware-esp32p4/managed_components/  — esp-dl, esp-detection, etc.
#    • ~/esp/esp-idf/components/       — IDF framework headers
#    • ~/.espressif/tools/riscv32-esp-elf/…  — RISC-V toolchain headers
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT="${REPO_ROOT}/firmware-esp32p4/build/compile_commands.json"
OUT_DIR="${REPO_ROOT}/build-esp32p4-clion"
OUTPUT="${OUT_DIR}/compile_commands.json"

if [[ ! -f "${INPUT}" ]]; then
    echo "ERROR: firmware-esp32p4/build/compile_commands.json not found." >&2
    echo "       Run first: bash scripts/build-esp32p4-mac.sh" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
cp "${INPUT}" "${OUTPUT}"

echo "Copied  : firmware-esp32p4/build/compile_commands.json"
echo "      → : build-esp32p4-clion/compile_commands.json"
echo
echo "In CLion: File → Open → ${OUT_DIR}"
echo "          (select 'Open as Project' when prompted)"
