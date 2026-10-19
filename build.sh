#!/usr/bin/env bash

# PS4-Linux Strawberry Builder
# Supports two PS4-focused build profiles and three LTO flavors:
#   server   — headless/services, HZ=250, PREEMPT_VOLUNTARY, performance governor
#   general  — desktop/gaming, HZ=250, PREEMPT=y, performance/reflex
#   slopmax  — general + KVM (CONFIG_KVM/KVM_AMD) for running VMs on top
#   slopium  — server + KVM (CONFIG_KVM/KVM_AMD) for running VMs on top
#   NoLTO / ThinLTO / FullLTO selectable via lto=NoLTO, lto=ThinLTO, lto=FullLTO
#
# Usage:
#   ./build.sh --option N
#   ./build.sh --option N use=Server
#   ./build.sh --option N lto=ThinLTO
#   ./build.sh --option N use=General lto=FullLTO jobs=8
#   ./build.sh --option N use=SlopMax
#   ./build.sh --option N use=Slopium
#     N: 1=build  2=fetch firmware  3=both

set -euo pipefail

OUTPUT_DIR="${PWD}/out"
FIRMWARE_DIR="${PWD}/extra_firmware"
FIRMWARE_URL_BASE="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main"
REQUIRED_PS4_SD8797_FW="mrvl/sd8797_uapsta.bin"
declare -A FIRMWARE_URL_OVERRIDES
FIRMWARE_URL_OVERRIDES["mrvl/sd8797_uapsta.bin"]="useCustomFirmware" # Prevent download from upstream; use Orbis/Custom one
#FIRMWARE_URL_OVERRIDES["mrvl/sd8797_uapsta.bin"]="f87c5b8dd547bcb434d5296ead3748241810c1d8" #sucks too
# We need an older firmware version from ~2013-2016 for Aeolias' 8797 SDIO Chip, ideally the one that's used on the PS4 OS.
# This version is the closest to that we have (besides the one packed in Orbis Torus (WiFi+BT) firmware).

# PS4 Jaguar tuning
export KCFLAGS="-march=btver2 -mtune=btver2 -O3"
export KAFLAGS="-march=btver2 -mtune=btver2"

export HOSTCFLAGS="-Wno-error=incompatible-pointer-types-discards-qualifiers"

PROFILE="server"
LTO_FLAVOR="thin"
JOBS="$(nproc)"
MAX_JOBS="$(nproc)"

lto_label() {
    if [[ "$LTO_FLAVOR" == "full" ]]; then
        echo "FullLTO"
    elif [[ "$LTO_FLAVOR" == "none" ]]; then
        echo "NoLTO"
    else
        echo "ThinLTO"
    fi
}

apply_config_file() {
    local file="$1"
    local action name value
    while read -r action name value; do
        [[ -z "$action" || "$action" == \#* ]] && continue
        case "$action" in
            enable)  scripts/config --enable  "$name" ;;
            disable) scripts/config --disable "$name" ;;
            set-val) scripts/config --set-val "$name" "$value" ;;
            set-str) scripts/config --set-str "$name" "$value" ;;
        esac
    done < "$file"
}

profile_label() {
    case "$PROFILE" in
        general) echo "General" ;;
        slopmax) echo "SlopMax" ;;
        slopium) echo "Slopium" ;;
        *)       echo "Server" ;;
    esac
}

ensure_extra_firmware_blob() {
    local blob="$1"
    local current=""

    if grep -qE '^CONFIG_EXTRA_FIRMWARE=' .config; then
        current="$(sed -n 's/^CONFIG_EXTRA_FIRMWARE="\(.*\)"/\1/p' .config)"
    fi

    case " ${current} " in
        *" ${blob} "*)
            ;;
        *)
            current="${current:+${current} }${blob}"
            scripts/config --set-str CONFIG_EXTRA_FIRMWARE "${current}"
            ;;
    esac
}

config_has_extra_firmware_blob() {
    local blob="$1"
    local current=""

    if grep -qE '^CONFIG_EXTRA_FIRMWARE=' .config; then
        current="$(sed -n 's/^CONFIG_EXTRA_FIRMWARE="\(.*\)"/\1/p' .config)"
    fi

    case " ${current} " in
        *" ${blob} "*)
            return 0
            ;;
    esac

    return 1
}

require_custom_firmware_blob() {
    local blob="$1"
    local path="${FIRMWARE_DIR}/${blob}"

    if [[ ! -f "${path}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m Required PS4 custom firmware is missing: ${path}" >&2
        echo "Place the proprietary SD8797 firmware at ${path}" >&2
        exit 1
    fi
}

validate_extra_firmware_blob() {
    local blob="$1"

    if ! config_has_extra_firmware_blob "${blob}"; then
        echo -e "\e[1;31mERROR:\e[0m CONFIG_EXTRA_FIRMWARE must include required PS4 custom firmware: ${blob}" >&2
        exit 1
    fi
}

print_disclaimer() {
    echo -e "\e[1;33mIf you want to contribute to this kernel: fork it and open a pull request.\e[0m"
    echo -e "\e[1;33mDo not base a kernel off this one, dump it as a .patch repo, and call it your own work.\e[0m"
}

print_usage() {
    echo "Usage: ./build.sh --option N [use=Profile] [lto=LTOFlavor] [jobs=N]"
    echo "  N: 1=build  2=fetch firmware  3=both"
    echo ""
    echo "  use=Server/use=General/use=SlopMax/use=Slopium  (default: Server)"
    echo "  lto=NoLTO/lto=ThinLTO/lto=FullLTO                (default: ThinLTO)"
    echo "  jobs=N                                           (default: nproc)"
    echo ""
    echo "Examples:"
    echo "  ./build.sh --option N use=Server"
    echo "  ./build.sh --option N lto=ThinLTO"
    echo "  ./build.sh --option N use=General lto=FullLTO jobs=8"
    echo "  ./build.sh --option N use=SlopMax"
    echo "  ./build.sh --option N use=Slopium"
}

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            print_usage
            echo ""
            print_disclaimer
            exit 0
            ;;
    esac
done

# Parse optional selectors in any position:
#   use=Server/use=General/use=SlopMax/use=Slopium
#   lto=NoLTO/lto=ThinLTO/lto=FullLTO
#   jobs=N
for arg in "$@"; do
    case "$arg" in
        use=*)
            PROFILE_ARG="${arg#use=}"
            case "${PROFILE_ARG,,}" in
                server) PROFILE="server" ;;
                general) PROFILE="general" ;;
                slopmax) PROFILE="slopmax" ;;
                slopium) PROFILE="slopium" ;;
                *)
                    echo "Unknown build profile: ${PROFILE_ARG}. Valid: Server, General, SlopMax, Slopium"
                    exit 1
                    ;;
            esac
            ;;
        lto=*)
            LTO_ARG="${arg#lto=}"
            case "${LTO_ARG,,}" in
                thinlto|thin) LTO_FLAVOR="thin" ;;
                fulllto|full) LTO_FLAVOR="full" ;;
                nolto|none|no|off|disabled) LTO_FLAVOR="none" ;;
                *)
                    echo "Unknown LTO flavor: ${LTO_ARG}. Valid: NoLTO, ThinLTO, FullLTO"
                    exit 1
                    ;;
            esac
            ;;
        jobs=*)
            JOBS_ARG="${arg#jobs=}"
            if [[ "$JOBS_ARG" =~ ^[0-9]+$ ]] && [[ "$JOBS_ARG" -ge 1 ]] && [[ "$JOBS_ARG" -le "$MAX_JOBS" ]]; then
                JOBS="$JOBS_ARG"
            else
                echo "Invalid jobs value: ${JOBS_ARG}. Must be 1-${MAX_JOBS}."
                exit 1
            fi
            ;;
    esac
done

if [[ $# -lt 2 || "$1" != "--option" ]]; then
    print_usage
    exit 1
fi

CHOICE="$2"
case "$CHOICE" in
    1) DO_BUILD=1; DO_FETCH=0 ;;
    2) DO_BUILD=0; DO_FETCH=1 ;;
    3) DO_BUILD=1; DO_FETCH=1 ;;
    *)
        echo "Invalid --option argument: $CHOICE. Must be 1, 2, or 3."
        exit 1
        ;;
esac

MAKE_OPTS=(
    -j"${JOBS}"
    LLVM=1
    ARCH=x86_64
    HOSTCFLAGS="${HOSTCFLAGS}"
)

if [[ ! -f Makefile ]] || ! grep -q "KERNELRELEASE" Makefile 2>/dev/null; then
    echo -e "\e[1;31mERROR:\e[0m Run this from the kernel source root." >&2
    exit 1
fi

# if [[ ! -f .config ]]; then  # We need to update the config file even if .config exists from a prev. cached build
if [[ -f config ]]; then
    echo -e "\e[1;34m[*]\e[0m Moving 'config' -> '.config'"
    mv config .config
else
    echo -e "\e[1;31mERROR:\e[0m No .config found." >&2
    exit 1
fi

ensure_extra_firmware_blob "${REQUIRED_PS4_SD8797_FW}"
require_custom_firmware_blob "${REQUIRED_PS4_SD8797_FW}"
validate_extra_firmware_blob "${REQUIRED_PS4_SD8797_FW}"
echo -e "\e[1;34m[*]\e[0m Setting CONFIG_EXTRA_FIRMWARE_DIR=${FIRMWARE_DIR}"
scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "${FIRMWARE_DIR}"

if [[ "$DO_FETCH" == "1" ]]; then
    CONFIG_LINE="$(grep -E '^CONFIG_EXTRA_FIRMWARE=' .config 2>/dev/null || true)"
    if [[ -z "${CONFIG_LINE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m CONFIG_EXTRA_FIRMWARE not found in .config" >&2
        exit 1
    fi

    BLOBS="$(echo "${CONFIG_LINE}" | sed 's/CONFIG_EXTRA_FIRMWARE="\(.*\)"/\1/' | tr ' ' '\n' | grep -v '^$' || true)"

    if [[ -z "${BLOBS}" ]]; then
        echo "CONFIG_EXTRA_FIRMWARE is empty -- nothing to fetch."
    else
        echo -e "\e[1;34m[*]\e[0m Blobs required by CONFIG_EXTRA_FIRMWARE:"
        echo "${BLOBS}" | sed 's/^/    /'
        echo ""
        mkdir -p "${FIRMWARE_DIR}"
        FAILED=()
        while IFS= read -r blob; do
            dest="${FIRMWARE_DIR}/${blob}"
            if [[ -f "${dest}" ]]; then
                echo -e "  \e[1;32m[=]\e[0m Already exists: ${blob}"
                continue
            fi
            mkdir -p "$(dirname "${dest}")"
            echo -e "  \e[1;34m[↓]\e[0m Fetching: ${blob}"

            FIRMWARE_URL="${FIRMWARE_URL_BASE}"
            FIRMWARE_URL_FALLBACK=""

            if [[ -n "${FIRMWARE_URL_OVERRIDES[$blob]:-}" ]]; then
                if [[ ${FIRMWARE_URL_OVERRIDES[$blob]} == "useCustomFirmware" ]]; then
                    echo -e "  \e[1;31mERROR:\e[0m Requested custom built-in firmware for ${blob}, but it was not found in build directory.\n"\
                            " Please ensure the proper firmware exists in ${FIRMWARE_DIR}/${blob} . Exiting with error." >&2
                    exit 1
                fi
                COMMIT="${FIRMWARE_URL_OVERRIDES[$blob]}"
                FIRMWARE_URL_FALLBACK="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/${COMMIT}"
            fi

            if [[ -n "${FIRMWARE_URL_FALLBACK}" ]]; then
                FIRMWARE_URL="${FIRMWARE_URL_FALLBACK}"
                echo -e '  \e[1;33m[!] Using non-default firmware for '${blob}'!\n  Using commit from '${FIRMWARE_URL}''
            fi

            if curl -fsSL --retry 3 --retry-delay 2 "${FIRMWARE_URL}/${blob}" -o "${dest}"; then
                echo -e "  \e[1;32m[✓]\e[0m ${blob}"
            else
                echo -e "  \e[1;31m[✗]\e[0m FAILED: ${blob}" >&2
                FAILED+=("${blob}")
                rm -f "${dest}"
            fi
        done <<< "${BLOBS}"

        echo ""
        if [[ ${#FAILED[@]} -eq 0 ]]; then
            echo -e "\e[1;32mAll firmware blobs fetched -> ${FIRMWARE_DIR}\e[0m"
        else
            echo -e "\e[1;31mThe following blobs could not be fetched:\e[0m"
            printf '  %s\n' "${FAILED[@]}"
            exit 1
        fi
    fi
fi

if [[ "$DO_BUILD" == "1" ]]; then
    print_disclaimer
    echo -e "\e[1;34m[*]\e[0m Applying invariant config..."

    LOCALVERSION_SUFFIX="-Strawberry-$(profile_label)-$(lto_label)"

    # Build system / LTO
    echo -e "\e[1;34m[*]\e[0m Enabling $(lto_label)..."
    apply_config_file "configs/lto-${LTO_FLAVOR}.cfg"
    scripts/config --disable CONFIG_LOCALVERSION_AUTO
    scripts/config --set-str CONFIG_LOCALVERSION "${LOCALVERSION_SUFFIX}"

    apply_config_file "configs/base.cfg"

    if [[ "$PROFILE" == "server" || "$PROFILE" == "slopium" ]]; then
        echo -e "\e[1;34m[*]\e[0m Applying server profile..."
        apply_config_file "configs/tuning-server.cfg"
    else
        echo -e "\e[1;34m[*]\e[0m Applying general/gaming profile..."
        apply_config_file "configs/tuning-general.cfg"
    fi

    if [[ "$PROFILE" == "slopmax" || "$PROFILE" == "slopium" ]]; then
        echo -e "\e[1;34m[*]\e[0m Applying KVM support..."
        apply_config_file "configs/kvm.cfg"
    fi

    echo -e "\e[1;34m[*]\e[0m Running olddefconfig..."
    make "${MAKE_OPTS[@]}" olddefconfig

    validate_extra_firmware_blob "${REQUIRED_PS4_SD8797_FW}"

    echo -e "\e[1;34m[*]\e[0m Running prepare..."
    make "${MAKE_OPTS[@]}" prepare

    CURRENT_LTO_LABEL="$(lto_label)"
    echo -e "\e[1;34m[*]\e[0m Building bzImage [profile: ${PROFILE}, LTO: ${CURRENT_LTO_LABEL}] with ${JOBS} jobs..."
    time make "${MAKE_OPTS[@]}" bzImage

    BZIMAGE="arch/x86/boot/bzImage"
    if [[ ! -f "${BZIMAGE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m bzImage not found after build." >&2
        exit 1
    fi

    mkdir -p "${OUTPUT_DIR}"
    cp "${BZIMAGE}" "${OUTPUT_DIR}/bzImage"
    cp .config "${OUTPUT_DIR}/.config"

    KVER="$(cat include/config/kernel.release 2>/dev/null || echo "unknown")"
    LTO_LABEL="${CURRENT_LTO_LABEL}"

    PROFILE_LABEL="$(profile_label)"
    KVER_BASE="${KVER%%-*}"

    ARTIFACT_BASENAME="${KVER_BASE}-Strawberry-${PROFILE_LABEL}-${LTO_LABEL}"
    printf '%s\n' "${ARTIFACT_BASENAME}" > "${OUTPUT_DIR}/artifact_name.txt"

    echo -e "\e[1;32m[✓]\e[0m Build complete [${PROFILE} / ${LTO_LABEL}] -> ${OUTPUT_DIR}/bzImage (${KVER})"
    echo ""
    print_disclaimer
fi
