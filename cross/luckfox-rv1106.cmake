# ─────────────────────────────────────────────────────────────────────────────
#  CMake toolchain file — Luckfox Pico Zero (RV1106G3, Cortex-A7)
#
#  Used when cross-compiling CMake projects (e.g. NCNN) for the target.
#  Pass to cmake with:
#    cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/cross/luckfox-rv1106.cmake ...
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_TC /home/ubuntu/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf)

set(CMAKE_C_COMPILER    ${_TC}-gcc)
set(CMAKE_CXX_COMPILER  ${_TC}-g++)
set(CMAKE_AR            ${_TC}-ar    CACHE FILEPATH "")
set(CMAKE_STRIP         ${_TC}-strip CACHE FILEPATH "")
set(CMAKE_OBJCOPY       ${_TC}-objcopy CACHE FILEPATH "")

# Cortex-A7 + NEON flags — match the Meson build
set(_ARM_FLAGS "-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer")
set(CMAKE_C_FLAGS_INIT   "${_ARM_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_ARM_FLAGS}")

# Only search target sysroot for libraries/headers, never the host system
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
