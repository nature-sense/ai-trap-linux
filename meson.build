# ─────────────────────────────────────────────────────────────────────────────
#  meson.build — Insect Trap Pipeline
#
#  Source layout:
#    firmware/src/   all C++ source and header files
#    trap_config.toml  runtime config (not compiled)
#
#  Build targets (set with -Dtarget=):
#    libcamera     Pi 5 + IMX708 via libcamera         → yolo_libcamera
#    v4l2          Luckfox Pico Zero + IMX415 via V4L2  → yolo_v4l2
#    all           both (requires libcamera)
#
#  Native build:
#    meson setup build -Dtarget=libcamera && ninja -C build
#    meson setup build -Dtarget=v4l2      && ninja -C build
#
#  Cross-compile for Raspberry Pi 5 (aarch64) on Ubuntu x86:
#    sudo dpkg --add-architecture arm64
#    sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#                         libcamera-dev:arm64
#    # Build SQLite3 into /opt/pi5-sysroot (see cross/pi5-aarch64.ini)
#    meson setup build-pi5 \
#      --cross-file cross/pi5-aarch64.ini \
#      -Dtarget=libcamera \
#      -Dpi5_sysroot=/opt/pi5-sysroot
#    ninja -C build-pi5
#
#  Cross-compile for Luckfox Pico Zero (RV1106G3) on Ubuntu x86:
#    # Install arm-rockchip830-linux-uclibcgnueabihf toolchain first
#    # Build SQLite3 + librknnmrt into /opt/luckfox-sysroot (see scripts/build-luckfox-mac.sh)
#    meson setup build-luckfox \
#      --cross-file cross/luckfox-rv1106.ini \
#      -Dtarget=v4l2 \
#      -Dluckfox_sysroot=/opt/luckfox-sysroot
#    ninja -C build-luckfox
# ─────────────────────────────────────────────────────────────────────────────

project('yolo-trap', 'cpp',
  default_options : ['cpp_std=c++17', 'optimization=2'])

# ── Cross-compilation: target detection ───────────────────────────────────────

is_arm_cross     = (meson.is_cross_build() and host_machine.cpu_family() == 'arm')
is_aarch64_cross = (meson.is_cross_build() and host_machine.cpu_family() == 'aarch64')

luckfox_sysroot = get_option('luckfox_sysroot')
pi5_sysroot     = get_option('pi5_sysroot')

# Resolve the active cross sysroot (empty string = not set)
_cross_sysroot = ''
if is_arm_cross and luckfox_sysroot != ''
  _cross_sysroot = luckfox_sysroot
  message('Luckfox (armv7) cross-build: sysroot = ' + luckfox_sysroot)
elif is_aarch64_cross and pi5_sysroot != ''
  _cross_sysroot = pi5_sysroot
  message('Pi 5 (aarch64) cross-build: sysroot = ' + pi5_sysroot)
endif

if _cross_sysroot != ''
  add_project_arguments(
    ['-isystem', _cross_sysroot + '/usr/include'],
    language : 'cpp')
  add_project_link_arguments(
    '-L' + _cross_sysroot + '/usr/lib',
    language : 'cpp')
endif

# ── CPU-specific compile flags ─────────────────────────────────────────────────
#
#  armv7  (Luckfox RV1106G3 / Cortex-A7):
#    -mfpu=neon-vfpv4  enables NEON intrinsics
#    -mfloat-abi=hard  matches the uClibc hard-float ABI
#
#  aarch64 (Raspberry Pi 5 / Cortex-A76):
#    +dotprod+fp16     enables ASIMD dot-product and FP16

if is_arm_cross
  _cpu_flags = [
    '-march=armv7-a',
    '-mtune=cortex-a7',
    '-mfpu=neon-vfpv4',
    '-mfloat-abi=hard',
    '-fomit-frame-pointer',
  ]
  add_project_arguments(_cpu_flags, language : 'cpp')
  add_project_arguments(_cpu_flags, language : 'c')
elif is_aarch64_cross
  _cpu_flags = [
    '-march=armv8.2-a+dotprod+fp16',
    '-mtune=cortex-a76',
  ]
  add_project_arguments(_cpu_flags, language : 'cpp')
  add_project_arguments(_cpu_flags, language : 'c')
endif

# ── Static C++ runtime for cross builds ───────────────────────────────────────
#
# Statically linking libstdc++ and libgcc_s produces a self-contained binary
# that runs on the target without matching the exact host toolchain version.
# glibc and libcamera remain dynamic (they are always present on the target OS).

if meson.is_cross_build()
  add_project_link_arguments(
    # Static C++ runtime — avoids toolchain version mismatch on target.
    '-static-libstdc++', '-static-libgcc',
    # libcamera.so's transitive deps (libpisp, libudev, libyaml, libdw, etc.)
    # are not in the staging sysroot but will be present on the Pi at runtime.
    # --allow-shlib-undefined suppresses linker errors for symbols that are
    # only needed by a shared library we link against (not by our code directly).
    '-Wl,--allow-shlib-undefined',
    language : 'cpp')
endif

# ── Dependencies ──────────────────────────────────────────────────────────────
#
# For cross-builds, SQLite3 is located directly via find_library() from the
# staging sysroot.  libcamera is always discovered via pkg-config since it is
# installed to the system multiarch path by apt.

cxx = meson.get_compiler('cpp')

thread_dep    = dependency('threads',   required : true)
omp_dep       = dependency('openmp',    required : false)
# libcamera pkg-config name changed across versions:
#   Ubuntu 22.04 (0.0.4)   → 'camera'
#   Raspberry Pi OS / newer → 'libcamera'
# Try the modern name first, fall back to the old one.
libcamera_dep = dependency('libcamera', required : false)
if not libcamera_dep.found()
  libcamera_dep = dependency('camera', required : false)
endif

if _cross_sysroot != ''
  _lib      = _cross_sysroot + '/usr/lib'
  _lib_arch = _cross_sysroot + '/usr/lib/aarch64-linux-gnu'

  sqlite3_dep = cxx.find_library('sqlite3', dirs : [_lib], required : true)

  # libcamera: if pkg-config already found it (e.g. via the sysroot
  # pkg-config wrapper) keep that result.  Only fall back to find_library
  # if pkg-config came up empty.
  if is_aarch64_cross and not libcamera_dep.found()
    libcamera_dep = cxx.find_library('camera',
                      dirs     : [_lib_arch, _lib],
                      required : false)
  endif

  if is_arm_cross
    # RKNN NPU mini-runtime (v4l2 and rkmpi targets).
    # librknnmrt.so + rknn_api.h fetched from rknn-toolkit2 by build-luckfox-mac.sh.
    # RV1106 uses the mini-runtime (librknnmrt), not the full librknnrt.
    rknn_dep = cxx.find_library('rknnmrt', dirs : [_lib], required : true)

    # RKMPI / Rockit media framework (rkmpi target only).
    # librockit.so + headers fetched from LuckfoxTECH/luckfox-pico SDK by
    # build-luckfox-mac.sh.  Already present on the device at /usr/lib/librockit.so.
    rockit_dep = cxx.find_library('rockit', dirs : [_lib], required : false)

    # libjpeg-turbo static — NEON-accelerated JPEG encoding for the MJPEG stream.
    # Built by build-luckfox-mac.sh and installed as a static lib into the sysroot.
    # Static link means no runtime dependency — binary is self-contained.
    jpeg_dep = cxx.find_library('jpeg', dirs : [_lib], required : false, static : true)
  endif
else
  sqlite3_dep = dependency('sqlite3', required : true)
endif

# Bluetooth headers — needed for BLE GATT server (Pi 5 and Luckfox).
# We use only macros/inline functions from these headers; no libbluetooth.so or
# libbluetooth.a is linked, so there is zero runtime dependency on BlueZ packages.
# Headers come from the sysroot (see pi5_sysroot / luckfox_sysroot options).
cc = meson.get_compiler('cpp')

# ── Option: select build target ───────────────────────────────────────────────

target_opt = get_option('target')   # 'libcamera' | 'v4l2' | 'rkmpi' | 'all'

# ── Shared sources (compiled into both targets) ───────────────────────────────

shared_sources = files(
  'firmware/src/pipeline/decoder.cpp',
  'firmware/src/pipeline/tracker.cpp',
  'firmware/src/pipeline/persistence.cpp',
  'firmware/src/pipeline/crop_saver.cpp',
  'firmware/src/pipeline/exif_writer.cpp',
  'firmware/src/pipeline/sync_manager.cpp',
  'firmware/src/server/mjpeg_streamer.cpp',
  'firmware/src/server/sse_server.cpp',
  'firmware/src/server/http_server.cpp',
  'firmware/src/common/stb_image_write_impl.cpp',
  'firmware/src/common/wifi_manager.cpp',
  'firmware/src/common/ble_gatt_server.cpp',
  'firmware/src/common/epaper_display.cpp',
)

shared_deps = [sqlite3_dep, thread_dep, omp_dep]

inc = include_directories([
                            'firmware/src/common',
                            'firmware/src/io',
                            'firmware/src/pipeline',
                            'firmware/src/server'
                          ])

# ── Pi 5 / libcamera target ───────────────────────────────────────────────────

if target_opt == 'libcamera' or (target_opt == 'all' and not is_arm_cross)
  if not libcamera_dep.found()
    error('target=libcamera requires libcamera. ' +
          'On the Luckfox cross-build host use -Dtarget=v4l2 instead.')
  endif
  executable('yolo_libcamera',
    sources : shared_sources + files(
      'firmware/src/io/libcamera_capture.cpp',
      'firmware/main_libcamera.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps + [libcamera_dep],
    cpp_args            : ['-DWIFI_BACKEND_NM'],
    # Bake /opt/ai-trap/lib into the ELF RPATH so the binary finds the
    # bundled libcamera shared libraries without LD_LIBRARY_PATH.
    install_rpath       : '/opt/ai-trap/lib',
  )
endif

# ── Luckfox Pico Zero / IMX415 / V4L2 target ─────────────────────────────────
#
# Stable baseline: V4L2 MMAP capture + software preprocessing + RKNN NPU.

if target_opt == 'v4l2' or target_opt == 'all'
  executable('yolo_v4l2',
    sources : shared_sources + files(
      'firmware/src/io/v4l2_capture.cpp',
      'firmware/src/io/rknn_infer.cpp',
      'firmware/main_v4l2.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps + [rknn_dep],
    cpp_args            : ['-DWIFI_BACKEND_HOSTAPD'],
  )
endif

# ── Luckfox Pico Zero / IMX415 / RKMPI+VPSS target ───────────────────────────
#
# Prototype: RKMPI VI + VPSS hardware scaling + async RKNN NPU inference.
#
# Key improvements over v4l2:
#   - VPSS (RGA2) scales 1920×1080 → 320×320 in hardware (~9 ms CPU saved/frame)
#   - VPSS delivers a second channel at 640×480 for streaming (no extra CPU)
#   - Async inference thread: capture preprocessing and rknn_run overlap
#   - rkipc must be stopped before launch (see S99trap-rkmpi in package/)
#
# Requires: librockit.so + RKMPI headers in sysroot (build-luckfox-mac.sh)

if target_opt == 'rkmpi' or target_opt == 'all'
  if not is_arm_cross or not rockit_dep.found()
    if target_opt == 'rkmpi'
      error('target=rkmpi requires librockit.so in the Luckfox sysroot. ' +
            'Run build-luckfox-mac.sh to fetch it.')
    endif
    # In target=all, skip rkmpi silently if rockit is not yet available.
  else
    executable('yolo_rkmpi',
      sources : shared_sources + files(
        'firmware/src/io/rkmpi_capture.cpp',
        'firmware/src/io/rknn_infer.cpp',
        'firmware/main_rkmpi.cpp',
      ),
      include_directories : inc,
      dependencies        : shared_deps + [rknn_dep, rockit_dep] +
                            (jpeg_dep.found() ? [jpeg_dep] : []),
      cpp_args            : ['-DWIFI_BACKEND_HOSTAPD'] +
                            (jpeg_dep.found() ? ['-DHAVE_LIBJPEG'] : []),
    )
  endif
endif
