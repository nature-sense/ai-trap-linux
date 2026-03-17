#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-luckfox-deps.sh
#
#  Cross-compiles SQLite3 and NCNN for the Luckfox Pico Zero (RV1106G3)
#  and installs them into a local sysroot.
#
#  Run from the repository root:
#    bash scripts/build-luckfox-deps.sh
#
#  On success the sysroot is at:
#    sysroot/luckfox/usr/lib/        — libncnn.a  libsqlite3.a
#    sysroot/luckfox/usr/include/    — headers
#    sysroot/luckfox/usr/lib/pkgconfig/ — .pc files
#
#  Then configure the project with:
#    meson setup build-luckfox \
#      --cross-file cross/luckfox-rv1106.ini \
#      -Dtarget=v4l2 \
#      -Dluckfox_sysroot="$(pwd)/sysroot/luckfox"
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_BIN="/home/ubuntu/arm-rockchip830-linux-uclibcgnueabihf/bin"
CROSS="arm-rockchip830-linux-uclibcgnueabihf"
SYSROOT="${REPO_ROOT}/sysroot/luckfox"
BUILD_DIR="${REPO_ROOT}/sysroot/.build"
JOBS="$(nproc)"

SQLITE_VERSION="3470200"   # 3.47.2  — update as needed
SQLITE_YEAR="2024"

ARM_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer"

export PATH="${TOOLCHAIN_BIN}:${PATH}"
export CC="${CROSS}-gcc"
export CXX="${CROSS}-g++"
export AR="${CROSS}-ar"
export STRIP="${CROSS}-strip"
export CFLAGS="${ARM_FLAGS} -O2"
export CXXFLAGS="${ARM_FLAGS} -O2"

# ── Helpers ───────────────────────────────────────────────────────────────────

banner() { echo; echo "────────────────────────────────────────"; echo "  $*"; echo "────────────────────────────────────────"; }
die()    { echo "ERROR: $*" >&2; exit 1; }

check_tool() {
    command -v "${CROSS}-gcc" >/dev/null 2>&1 || \
        die "Toolchain not found at ${TOOLCHAIN_BIN}. Check TOOLCHAIN_BIN in this script."
}

# ── SQLite3 ───────────────────────────────────────────────────────────────────

build_sqlite() {
    banner "Building SQLite3 ${SQLITE_VERSION}"

    local src="${BUILD_DIR}/sqlite-autoconf-${SQLITE_VERSION}"
    local tarball="${BUILD_DIR}/sqlite-autoconf-${SQLITE_VERSION}.tar.gz"

    mkdir -p "${BUILD_DIR}"

    if [[ ! -f "${tarball}" ]]; then
        echo "Downloading SQLite3..."
        curl -fSL \
            "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-autoconf-${SQLITE_VERSION}.tar.gz" \
            -o "${tarball}"
    fi

    [[ -d "${src}" ]] && rm -rf "${src}"
    tar xf "${tarball}" -C "${BUILD_DIR}"

    pushd "${src}" >/dev/null
    ./configure \
        --host="${CROSS}" \
        --prefix="/usr" \
        --enable-static \
        --disable-shared \
        --disable-readline \
        --disable-tcl \
        CFLAGS="${CFLAGS}"

    make -j"${JOBS}"
    make DESTDIR="${SYSROOT}" install
    popd >/dev/null

    echo "SQLite3 installed to ${SYSROOT}/usr"
}

# ── NCNN ──────────────────────────────────────────────────────────────────────

build_ncnn() {
    banner "Building NCNN"

    local src="${BUILD_DIR}/ncnn"
    local build="${BUILD_DIR}/ncnn-build"

    if [[ ! -d "${src}/.git" ]]; then
        echo "Cloning NCNN..."
        git clone --depth 1 https://github.com/Tencent/ncnn.git "${src}"
    else
        echo "NCNN source already present, skipping clone."
    fi

    [[ -d "${build}" ]] && rm -rf "${build}"
    mkdir -p "${build}"

    cmake -S "${src}" -B "${build}" \
        -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/cross/luckfox-rv1106.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${SYSROOT}/usr" \
        -DNCNN_SHARED_LIB=OFF \
        -DNCNN_BUILD_TESTS=OFF \
        -DNCNN_BUILD_EXAMPLES=OFF \
        -DNCNN_BUILD_TOOLS=OFF \
        -DNCNN_BUILD_BENCHMARK=OFF \
        -DNCNN_VULKAN=OFF \
        -DNCNN_TARGET_ARCH=arm \
        -DNCNN_ARM82=OFF \
        -DNCNN_OPENMP=OFF

    cmake --build "${build}" -j"${JOBS}"
    cmake --install "${build}"

    echo "NCNN installed to ${SYSROOT}/usr"
}

# ── Summary ───────────────────────────────────────────────────────────────────

print_summary() {
    banner "Done"
    echo "Sysroot : ${SYSROOT}"
    echo ""
    echo "Next step — configure the project:"
    echo ""
    echo "  meson setup build-luckfox \\"
    echo "    --cross-file cross/luckfox-rv1106.ini \\"
    echo "    -Dtarget=v4l2 \\"
    echo "    -Dluckfox_sysroot='${SYSROOT}'"
    echo ""
    echo "  ninja -C build-luckfox"
    echo ""
    echo "Deploy:"
    echo "  ${CROSS}-strip build-luckfox/yolo_v4l2"
    echo "  scp build-luckfox/yolo_v4l2 root@<board-ip>:/opt/trap/"
}

# ── Main ──────────────────────────────────────────────────────────────────────

check_tool
build_sqlite
build_ncnn
print_summary
