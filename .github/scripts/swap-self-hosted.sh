#!/usr/bin/env bash

set -euo pipefail

MODE="$1"
SWAP_GB="${2:-16}"
SWAP_FILE="${HOME}/.ci-swapfile"

case "$MODE" in
    enable)
        if swapon --show=NAME --noheadings | grep -qx "${SWAP_FILE}"; then
            echo "swapfile already active, skipping"
            exit 0
        fi
        sudo fallocate -l "${SWAP_GB}G" "${SWAP_FILE}"
        sudo chmod 600 "${SWAP_FILE}"
        sudo mkswap "${SWAP_FILE}"
        sudo swapon "${SWAP_FILE}"
        swapon --show
        ;;
    disable)
        if swapon --show=NAME --noheadings | grep -qx "${SWAP_FILE}"; then
            sudo swapoff "${SWAP_FILE}"
        fi
        sudo rm -f "${SWAP_FILE}"
        ;;
    *)
        echo "usage: swap-self-hosted.sh enable|disable [size-gb]" >&2
        exit 1
        ;;
esac
