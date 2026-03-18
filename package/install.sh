#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  install.sh — AI Trap installation script for Raspberry Pi 5 (aarch64)
#
#  Usage:
#    sudo ./install.sh
#
#  Installs to /opt/ai-trap and registers a systemd service.
#  An existing trap_config.toml is never overwritten.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

INSTALL_DIR="/opt/ai-trap"
SERVICE_NAME="ai-trap"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
BINARY_NAME="yolo_libcamera"
CONFIG_NAME="trap_config.toml"
SERVICE_USER="ai-trap"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[ai-trap]${NC} $*"; }
warn()  { echo -e "${YELLOW}[ai-trap]${NC} $*"; }
error() { echo -e "${RED}[ai-trap]${NC} $*" >&2; exit 1; }

# ── Pre-flight checks ─────────────────────────────────────────────────────────

[ "$(id -u)" -eq 0 ] || error "Please run as root: sudo ./install.sh"

ARCH=$(uname -m)
[ "$ARCH" = "aarch64" ] || error "This package is for aarch64 (Raspberry Pi 5). Detected: $ARCH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "${SCRIPT_DIR}/bin/${BINARY_NAME}" ]    || error "Binary not found: ${SCRIPT_DIR}/bin/${BINARY_NAME}"
[ -f "${SCRIPT_DIR}/systemd/${SERVICE_NAME}.service" ] || error "Service file not found"

# ── Create system user ────────────────────────────────────────────────────────

if ! id "${SERVICE_USER}" &>/dev/null; then
    info "Creating system user '${SERVICE_USER}'..."
    useradd --system --no-create-home --shell /usr/sbin/nologin \
            --groups video "${SERVICE_USER}"
else
    info "User '${SERVICE_USER}' already exists — skipping."
    # Ensure the user is in the video group for camera access
    usermod -aG video "${SERVICE_USER}" 2>/dev/null || true
fi

# ── Create directory structure ────────────────────────────────────────────────

info "Creating ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}/bin"
mkdir -p "${INSTALL_DIR}/models"
mkdir -p "${INSTALL_DIR}/crops"
mkdir -p "${INSTALL_DIR}/logs"
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${INSTALL_DIR}"
chmod 755 "${INSTALL_DIR}"

# ── Install binary ────────────────────────────────────────────────────────────

info "Installing binary..."
install -o root -g root -m 755 \
    "${SCRIPT_DIR}/bin/${BINARY_NAME}" \
    "${INSTALL_DIR}/bin/${BINARY_NAME}"

# ── Install config (never overwrite an existing one) ─────────────────────────

if [ -f "${INSTALL_DIR}/${CONFIG_NAME}" ]; then
    warn "Config already exists at ${INSTALL_DIR}/${CONFIG_NAME} — not overwriting."
    warn "Reference config saved to ${INSTALL_DIR}/${CONFIG_NAME}.new"
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/config/${CONFIG_NAME}" \
        "${INSTALL_DIR}/${CONFIG_NAME}.new"
else
    info "Installing default config..."
    install -o "${SERVICE_USER}" -g "${SERVICE_USER}" -m 644 \
        "${SCRIPT_DIR}/config/${CONFIG_NAME}" \
        "${INSTALL_DIR}/${CONFIG_NAME}"
fi

# ── Install systemd service ───────────────────────────────────────────────────

info "Installing systemd service..."
install -o root -g root -m 644 \
    "${SCRIPT_DIR}/systemd/${SERVICE_NAME}.service" \
    "${SERVICE_FILE}"

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}.service"

# ── Check for model files ─────────────────────────────────────────────────────

MODEL_PARAM="${INSTALL_DIR}/models/yolo11n-320/model.ncnn.param"
MODEL_BIN="${INSTALL_DIR}/models/yolo11n-320/model.ncnn.bin"

if [ ! -f "${MODEL_PARAM}" ] || [ ! -f "${MODEL_BIN}" ]; then
    warn "────────────────────────────────────────────────────────"
    warn "Model files not found. Before starting the service,"
    warn "copy your NCNN model to:"
    warn "  ${INSTALL_DIR}/models/yolo11n-320/model.ncnn.param"
    warn "  ${INSTALL_DIR}/models/yolo11n-320/model.ncnn.bin"
    warn ""
    warn "Then start the service with:"
    warn "  sudo systemctl start ${SERVICE_NAME}"
    warn "────────────────────────────────────────────────────────"
else
    info "Model files found — starting service..."
    systemctl start "${SERVICE_NAME}.service"
    systemctl status "${SERVICE_NAME}.service" --no-pager || true
fi

# ── Done ──────────────────────────────────────────────────────────────────────

info "Installation complete."
info ""
info "Useful commands:"
info "  sudo systemctl status  ${SERVICE_NAME}   — check status"
info "  sudo systemctl stop    ${SERVICE_NAME}   — stop"
info "  sudo systemctl start   ${SERVICE_NAME}   — start"
info "  sudo journalctl -u     ${SERVICE_NAME} -f  — follow logs"
info ""
info "Config:  ${INSTALL_DIR}/${CONFIG_NAME}"
info "Crops:   ${INSTALL_DIR}/crops/"
info "DB:      ${INSTALL_DIR}/detections.db"
info "API:     http://<pi-ip>:8080"
info "Stream:  http://<pi-ip>:9000"
