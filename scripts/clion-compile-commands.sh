#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  clion-compile-commands.sh
#
#  Generates a CLion-compatible compile_commands.json from the Docker build
#  output by remapping Docker-internal paths to real Mac paths.
#
#  Run this after build-luckfox-mac.sh:
#    bash scripts/clion-compile-commands.sh
#
#  Then in CLion:
#    File → Open → select build-clion/compile_commands.json
#    (CLion detects it as a compilation database and opens as a project)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT="${REPO_ROOT}/build-luckfox/compile_commands.json"
OUT_DIR="${REPO_ROOT}/build-clion"
OUTPUT="${OUT_DIR}/compile_commands.json"
CACHE="${HOME}/.cache/ai-trap/luckfox"

if [[ ! -f "${INPUT}" ]]; then
    echo "ERROR: build-luckfox/compile_commands.json not found." >&2
    echo "       Run first: bash scripts/build-luckfox-mac.sh" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

# Use Python for token-level path remapping so that only path tokens that
# *start* with /src/ or /cache/ are rewritten — sed would also corrupt paths
# like firmware/src/pipeline/ that contain /src/ in the middle.
python3 - "${REPO_ROOT}" "${CACHE}" "${INPUT}" "${OUTPUT}" <<'EOF'
import json, sys

repo_root, cache_dir, src, dst = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

def remap(p):
    if p.startswith('/src/'):
        return repo_root + '/' + p[5:]
    if p == '/src':
        return repo_root
    if p.startswith('/cache/'):
        return cache_dir + '/' + p[7:]
    return p

def remap_command(cmd):
    return ' '.join(remap(t) for t in cmd.split(' '))

with open(src) as f:
    db = json.load(f)

for e in db:
    if 'directory' in e: e['directory'] = remap(e['directory'])
    if 'file'      in e: e['file']      = remap(e['file'])
    if 'command'   in e: e['command']   = remap_command(e['command'])
    if 'arguments' in e: e['arguments'] = [remap(a) for a in e['arguments']]

with open(dst, 'w') as f:
    json.dump(db, f, indent=2)
EOF

echo "Written : build-clion/compile_commands.json"
echo
echo "In CLion: File → Open → ${OUTPUT} → 'Open as Project'"
