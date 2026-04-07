cmake_minimum_required(VERSION 3.16)

# ── ESP-IDF target ────────────────────────────────────────────────────────────
# Build:  idf.py set-target esp32p4 && idf.py build
# Flash:  idf.py -p /dev/ttyUSB0 flash monitor
# ─────────────────────────────────────────────────────────────────────────────

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai-trap-esp32p4 VERSION 0.1.0)
