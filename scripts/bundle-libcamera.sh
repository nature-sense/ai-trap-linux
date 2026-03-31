#!/usr/bin/env bash
# Bundle libcamera runtime files into package/lib for deploy.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "${REPO}/package/lib/ipa"
mkdir -p "${REPO}/package/share/libcamera/ipa/rpi/pisp"

cp /usr/lib/aarch64-linux-gnu/libcamera.so.0.7.0                   "${REPO}/package/lib/"
cp /usr/lib/aarch64-linux-gnu/libcamera-base.so.0.7.0              "${REPO}/package/lib/"
cp /usr/lib/aarch64-linux-gnu/libcamera/ipa/ipa_rpi_pisp.so        "${REPO}/package/lib/ipa/"
cp /usr/lib/aarch64-linux-gnu/libcamera/ipa/ipa_rpi_pisp.so.sign   "${REPO}/package/lib/ipa/"
cp /usr/share/libcamera/ipa/rpi/pisp/*.json                        "${REPO}/package/share/libcamera/ipa/rpi/pisp/"
