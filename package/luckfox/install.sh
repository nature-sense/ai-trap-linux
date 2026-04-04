#!/bin/sh
# install.sh  —  Deploy ai-trap to a Luckfox Pico Zero (run on the device)
#
# Usage:
#   scp -r package/luckfox root@<luckfox-ip>:/tmp/ai-trap-install
#   ssh root@<luckfox-ip> "sh /tmp/ai-trap-install/install.sh"

set -e

INSTALL_DIR=/opt/trap
INIT_D=/etc/init.d
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== ai-trap Luckfox installation (RKNN NPU) ==="

# ── Create install directory ──────────────────────────────────────────────────

mkdir -p "$INSTALL_DIR/crops"
echo "Install dir: $INSTALL_DIR"

# ── Binary ────────────────────────────────────────────────────────────────────

if [ -f "$SCRIPT_DIR/yolo_v4l2" ]; then
    cp "$SCRIPT_DIR/yolo_v4l2" "$INSTALL_DIR/yolo_v4l2"
    chmod +x "$INSTALL_DIR/yolo_v4l2"
    echo "Installed: yolo_v4l2"
else
    echo "WARNING: yolo_v4l2 not found in $SCRIPT_DIR — copy it manually"
fi

# ── RKNN model ────────────────────────────────────────────────────────────────

if [ -f "$SCRIPT_DIR/model.rknn" ]; then
    cp "$SCRIPT_DIR/model.rknn" "$INSTALL_DIR/model.rknn"
    echo "Installed: model.rknn"
else
    echo "WARNING: model.rknn not found — copy it to $INSTALL_DIR manually"
fi

# ── RKNN runtime library ──────────────────────────────────────────────────────

if [ -f "$SCRIPT_DIR/librknnmrt.so" ]; then
    cp "$SCRIPT_DIR/librknnmrt.so" "$INSTALL_DIR/librknnmrt.so"
    echo "Installed: librknnmrt.so"
else
    # Check if the library is already present on the system (Buildroot rootfs)
    if [ -f /usr/lib/librknnmrt.so ] || [ -f /lib/librknnmrt.so ]; then
        echo "librknnmrt.so already present on system"
    else
        echo "WARNING: librknnmrt.so not found — RKNN inference will fail at runtime"
        echo "         Deploy using: bash scripts/luckfox-install.sh (handles this automatically)"
    fi
fi

# ── Configuration ─────────────────────────────────────────────────────────────

if [ -f "$SCRIPT_DIR/trap_config.toml" ]; then
    cp "$SCRIPT_DIR/trap_config.toml" "$INSTALL_DIR/trap_config.toml"
    echo "Installed: trap_config.toml"
fi

# ── WiFi init script ─────────────────────────────────────────────────────────

cp "$SCRIPT_DIR/S50wifi" "$INIT_D/S50wifi"
chmod +x "$INIT_D/S50wifi"
echo "Installed: $INIT_D/S50wifi"

# ── Trap service init script ─────────────────────────────────────────────────

cat > "$INIT_D/S99trap" << 'EOF'
#!/bin/sh
# /etc/init.d/S99trap — ai-trap RKNN NPU build

INSTALL_DIR=/opt/trap
BINARY=$INSTALL_DIR/yolo_v4l2
LOGFILE=/var/log/ai-trap.log
PIDFILE=/var/run/ai-trap.pid

# RKNN runtime is in /opt/trap — add to dynamic linker search path
export LD_LIBRARY_PATH=/opt/trap${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

case "$1" in
  start)
    echo "Starting ai-trap (RKNN NPU)..."
    cd "$INSTALL_DIR"
    start-stop-daemon -S -b -m -p "$PIDFILE" \
        --exec "$BINARY" -- \
        "$INSTALL_DIR/model.rknn" \
        "$INSTALL_DIR/detections.db" \
        "$INSTALL_DIR/crops" \
        /dev/video0 \
        >> "$LOGFILE" 2>&1
    echo "ai-trap started (PID=$(cat $PIDFILE 2>/dev/null))"
    ;;
  stop)
    echo "Stopping ai-trap..."
    start-stop-daemon -K -p "$PIDFILE" --exec "$BINARY" 2>/dev/null || true
    rm -f "$PIDFILE"
    ;;
  restart)
    "$0" stop; sleep 1; "$0" start
    ;;
  status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "ai-trap is running (PID=$(cat $PIDFILE))"
    else
        echo "ai-trap is not running"
        exit 1
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 1
    ;;
esac
exit 0
EOF

chmod +x "$INIT_D/S99trap"
echo "Installed: $INIT_D/S99trap"

echo ""
echo "=== Installation complete ==="
echo ""
echo "WiFi provisioning:"
echo "  First boot starts in AP mode: SSID=ai-trap-luckfox_001, password=aiwildlife"
echo "  Connect your phone and POST to http://192.168.4.1:8080/api/wifi"
echo "  Body: {\"ssid\":\"YourNetwork\",\"password\":\"YourPass\"}"
echo "  To return to AP: POST http://<ip>:8080/api/wifi/reset"
echo ""
echo "Reboot to start services, or run:"
echo "  $INIT_D/S50wifi start && $INIT_D/S99trap start"
