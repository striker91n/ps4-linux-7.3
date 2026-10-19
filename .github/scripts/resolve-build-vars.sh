#!/usr/bin/env bash

set -euo pipefail

profile_input="$(echo "${1:-Server}" | tr '[:upper:]' '[:lower:]')"
lto_input="$(echo "${2:-ThinLTO}" | tr '[:upper:]' '[:lower:]')"

case "$profile_input" in
    general)  target_profile="General" ;;
    slopmax)  target_profile="SlopMax" ;;
    slopium)  target_profile="Slopium" ;;
    *)        target_profile="Server" ;;
esac

case "$lto_input" in
    fulllto|full)      target_lto="FullLTO" ;;
    nolto|none|no|off) target_lto="NoLTO" ;;
    *)                 target_lto="ThinLTO" ;;
esac

case "${target_lto}:${target_profile}" in
    NoLTO:Server)     swap_gb="16" ;;
    NoLTO:General)    swap_gb="20" ;;
    NoLTO:SlopMax)    swap_gb="22" ;;
    NoLTO:Slopium)    swap_gb="18" ;;
    ThinLTO:Server)   swap_gb="16" ;;
    ThinLTO:General)  swap_gb="20" ;;
    ThinLTO:SlopMax)  swap_gb="22" ;;
    ThinLTO:Slopium)  swap_gb="18" ;;
    FullLTO:Server)   swap_gb="24" ;;
    FullLTO:General)  swap_gb="28" ;;
    FullLTO:SlopMax)  swap_gb="30" ;;
    FullLTO:Slopium)  swap_gb="26" ;;
esac

echo "target_profile=${target_profile}"
echo "target_lto=${target_lto}"
echo "cache_suffix=${target_profile}-${target_lto}"
echo "swap_gb=${swap_gb}"
