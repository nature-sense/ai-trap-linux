#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  deploy.sh — build, bundle and push to a remote trap over SSH
#
#  Usage:
#    ./scripts/deploy.sh trap@trap001.local
#    ./scripts/deploy.sh trap@192.168.1.42
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

TARGET="${1:?Usage: $0 user@host}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${REPO}/buildDir"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[deploy]${NC} $*"; }
warn()  { echo -e "${YELLOW}[deploy]${NC} $*"; }
error() { echo -e "${RED}[deploy]${NC} $*" >&2; exit 1; }

# ── 1. Build ──────────────────────────────────────────────────────────────────

info "Building yolo_libcamera..."
cd "${BUILD}"
ninja -j$(nproc) 2>&1 | tail -5

BINARY="${BUILD}/yolo_libcamera"
[ -f "${BINARY}" ] || error "Binary not found after build: ${BINARY}"

# ── 2. Bake RPATH ─────────────────────────────────────────────────────────────

info "Setting RPATH → /opt/ai-trap/lib"
patchelf --set-rpath /opt/ai-trap/lib "${BINARY}"

# ── 3. Collect libcamera runtime files ───────────────────────────────────────

info "Bundling libcamera runtime..."
bash "${REPO}/scripts/bundle-libcamera.sh"

# ── 4. Create tarball ─────────────────────────────────────────────────────────

info "Creating deployment tarball..."
TMPDIR_DEPLOY=$(mktemp -d)
trap 'rm -rf "${TMPDIR_DEPLOY}"' EXIT

STAGE="${TMPDIR_DEPLOY}/ai-trap"
mkdir -p "${STAGE}/bin" "${STAGE}/lib/ipa" "${STAGE}/tuning"

cp "${BINARY}"                                               "${STAGE}/bin/yolo_libcamera"
cp "${REPO}/package/lib/libcamera.so.0.7.0"                 "${STAGE}/lib/"
cp "${REPO}/package/lib/libcamera-base.so.0.7.0"            "${STAGE}/lib/"
cp "${REPO}/package/lib/ipa/ipa_rpi_pisp.so"                "${STAGE}/lib/ipa/"
cp "${REPO}/package/lib/ipa/ipa_rpi_pisp.so.sign"           "${STAGE}/lib/ipa/"
cp "${REPO}/package/share/libcamera/ipa/rpi/pisp/"*.json    "${STAGE}/tuning/"
cp "${REPO}/package/ai-trap.service"                        "${STAGE}/"

TAR="${TMPDIR_DEPLOY}/ai-trap-deploy.tar.gz"
tar -czf "${TAR}" -C "${TMPDIR_DEPLOY}" ai-trap
info "Tarball: $(du -sh "${TAR}" | cut -f1)"

# ── 5. Upload and install ─────────────────────────────────────────────────────

info "Uploading to ${TARGET}..."
scp "${TAR}" "${TARGET}:/tmp/ai-trap-deploy.tar.gz"

info "Installing on ${TARGET}..."
ssh "${TARGET}" 'bash -s' << 'REMOTE'
set -euo pipefail

cd /tmp
rm -rf ai-trap
tar -xzf ai-trap-deploy.tar.gz

sudo systemctl stop ai-trap 2>/dev/null || true

# Binary
sudo cp ai-trap/bin/yolo_libcamera /opt/ai-trap/bin/yolo_libcamera
sudo chmod 755 /opt/ai-trap/bin/yolo_libcamera

# Bundled libcamera libs
sudo mkdir -p /opt/ai-trap/lib/ipa
sudo cp ai-trap/lib/libcamera.so.0.7.0       /opt/ai-trap/lib/
sudo cp ai-trap/lib/libcamera-base.so.0.7.0  /opt/ai-trap/lib/
sudo cp ai-trap/lib/ipa/ipa_rpi_pisp.so      /opt/ai-trap/lib/ipa/
sudo cp ai-trap/lib/ipa/ipa_rpi_pisp.so.sign /opt/ai-trap/lib/ipa/

# Create soname symlinks
sudo ldconfig -n /opt/ai-trap/lib

# Camera tuning JSON files
sudo mkdir -p /usr/share/libcamera/ipa/rpi/pisp
sudo cp ai-trap/tuning/*.json /usr/share/libcamera/ipa/rpi/pisp/

# Service file
sudo cp ai-trap/ai-trap.service /etc/systemd/system/ai-trap.service
sudo systemctl daemon-reload

echo ""
echo "=== /opt/ai-trap/lib ==="
ls -lh /opt/ai-trap/lib/ /opt/ai-trap/lib/ipa/
echo ""
echo "=== ldd check ==="
ldd /opt/ai-trap/bin/yolo_libcamera | grep -E "libcamera|not found"
echo ""

sudo systemctl start ai-trap
sleep 2
sudo journalctl -u ai-trap -n 15 --no-pager
REMOTE

info "Done."
