#!/usr/bin/env bash

set -euo pipefail

FW_REPO_PAT="$1"
KERNEL_SOURCE_DIR="$2"

git clone --depth=1 \
    "https://x-access-token:${FW_REPO_PAT}@github.com/sony-jaguar-devs/firmware.git" \
    /tmp/jaguar-fw

mkdir -p "${KERNEL_SOURCE_DIR}/extra_firmware/mrvl"
cp /tmp/jaguar-fw/sd8797_uapsta.bin "${KERNEL_SOURCE_DIR}/extra_firmware/mrvl/sd8797_uapsta.bin"
test -s "${KERNEL_SOURCE_DIR}/extra_firmware/mrvl/sd8797_uapsta.bin"

rm -rf /tmp/jaguar-fw
