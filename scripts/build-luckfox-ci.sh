#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-luckfox-ci.sh
#
#  Triggers the GitHub Actions Luckfox build on the current branch, waits
#  for it to complete, and downloads the binary to build-luckfox/yolo_v4l2.
#
#  Requirements:
#    gh  — GitHub CLI (brew install gh)
#
#  Usage:
#    bash scripts/build-luckfox-ci.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

WORKFLOW="build-luckfox.yml"
OUT_DIR="${REPO_ROOT}/build-luckfox"

# ── Prerequisites ─────────────────────────────────────────────────────────────

if ! command -v gh &>/dev/null; then
    echo "ERROR: GitHub CLI not found." >&2
    echo "       brew install gh" >&2
    exit 1
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
COMMIT="$(git rev-parse HEAD)"

echo "Branch : ${BRANCH}"
echo "Commit : ${COMMIT}"

# ── Check for unpushed commits ────────────────────────────────────────────────

if ! git diff --quiet "origin/${BRANCH}" HEAD 2>/dev/null; then
    echo
    echo "WARNING: local commits are not pushed — CI will build the last pushed commit."
    echo "         Run: git push"
    echo
    read -rp "Continue anyway? [y/N] " ans
    [[ "${ans}" =~ ^[Yy]$ ]] || exit 1
fi

# ── Trigger workflow ──────────────────────────────────────────────────────────

echo
echo "==> Triggering ${WORKFLOW} on ${BRANCH}..."
gh workflow run "${WORKFLOW}" --ref "${BRANCH}"

# Give GitHub a moment to register the run before we query for it
sleep 3

# Find the run ID we just created (most recent in-progress run on this branch)
RUN_ID="$(gh run list \
    --workflow "${WORKFLOW}" \
    --branch "${BRANCH}" \
    --limit 1 \
    --json databaseId \
    --jq '.[0].databaseId')"

echo "==> Run ID: ${RUN_ID}"
echo "    https://github.com/$(gh repo view --json nameWithOwner -q .nameWithOwner)/actions/runs/${RUN_ID}"

# ── Wait for completion ───────────────────────────────────────────────────────

echo
echo "==> Waiting for build to complete..."
gh run watch "${RUN_ID}" --exit-status

# ── Download artifact ─────────────────────────────────────────────────────────

echo
echo "==> Downloading artifact..."
mkdir -p "${OUT_DIR}"
TMPDIR="$(mktemp -d)"
gh run download "${RUN_ID}" --dir "${TMPDIR}"

# The artifact is a directory named yolo_v4l2-rv1106-<sha> containing yolo_v4l2
BINARY="$(find "${TMPDIR}" -name "yolo_v4l2" -type f | head -1)"
if [[ -z "${BINARY}" ]]; then
    echo "ERROR: yolo_v4l2 binary not found in downloaded artifact" >&2
    ls -R "${TMPDIR}" >&2
    rm -rf "${TMPDIR}"
    exit 1
fi

cp "${BINARY}" "${OUT_DIR}/yolo_v4l2"
chmod +x "${OUT_DIR}/yolo_v4l2"
rm -rf "${TMPDIR}"

echo
echo "Done  : build-luckfox/yolo_v4l2"
echo
echo "Deploy:"
echo "  scp build-luckfox/yolo_v4l2 root@<board-ip>:/opt/trap/"
