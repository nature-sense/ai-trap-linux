#!/bin/sh
# install.sh  —  Deploy ai-trap to a Luckfox Pico Zero (run on the device)
#
# Copies the binary, model files, and init scripts.
# Run as root from the directory containing this script.
#
# Usage:
#   scp -r package/luckfox root@<luckfox-ip>:/tmp/ai-trap-install
#   ssh root@<luckfox-ip> "sh /tmp/ai-trap-install/install.sh"

set -e

INSTALL_DIR=/opt/trap
INIT_D=/etc/init.d
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== ai-trap Luckfox installation ==="

# ── Create install directory ──────────────────────────────────────────────────
mkdir -p "$INSTALL_DIR/crops"
echo "Install dir: $INSTALL_DIR"

# ── Binary ───────────────────────────────────────────────────────────────────
if [ -f "$SCRIPT_DIR/yolo_v4l2" ]; then
    cp "$SCRIPT_DIR/yolo_v4l2" "$INSTALL_DIR/yolo_v4l2"
    chmod +x "$INSTALL_DIR/yolo_v4l2"
    echo "Installed: yolo_v4l2"
else
    echo "WARNING: yolo_v4l2 not found in $SCRIPT_DIR — copy it manually"
fi

# ── Model files ───────────────────────────────────────────────────────────────
for f in model.ncnn.param model.ncnn.bin; do
    if [ -f "$SCRIPT_DIR/$f" ]; then
        cp "$SCRIPT_DIR/$f" "$INSTALL_DIR/$f"
        echo "Installed: $f"
    else
        echo "WARNING: $f not found — copy model files to $INSTALL_DIR manually"
    fi
done

# ── WiFi init script ─────────────────────────────────────────────────────────
cp "$SCRIPT_DIR/S50wifi" "$INIT_D/S50wifi"
chmod +x "$INIT_D/S50wifi"
echo "Installed: $INIT_D/S50wifi"

# ── Trap service init script ─────────────────────────────────────────────────
cat > "$INIT_D/S99trap" << 'EOF'
#!/bin/sh
# /etc/init.d/S99trap — start ai-trap after WiFi is up

INSTALL_DIR=/opt/trap
BINARY=$INSTALL_DIR/yolo_v4l2
LOGFILE=/var/log/ai-trap.log
PIDFILE=/var/run/ai-trap.pid

case "$1" in
  start)
    echo "Starting ai-trap..."
    start-stop-daemon -S -b -m -p "$PIDFILE" \
        --exec "$BINARY" -- \
        "$INSTALL_DIR/model.ncnn.param" \
        "$INSTALL_DIR/model.ncnn.bin" \
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
  *)
    echo "Usage: $0 {start|stop|restart}"
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
