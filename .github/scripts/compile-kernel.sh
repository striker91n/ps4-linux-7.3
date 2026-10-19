#!/usr/bin/env bash

set -e

TARGET_PROFILE="$1"
TARGET_LTO="$2"

echo "Resetting kernel config to pristine..."
git checkout -- config

echo "Executing build.sh with profile: ${TARGET_PROFILE}"
echo "Executing build.sh with LTO: ${TARGET_LTO}"

chmod +x build.sh
./build.sh --option 3 use="${TARGET_PROFILE}" lto="${TARGET_LTO}"

echo "Packaging build artifacts..."

ARTIFACT_BASENAME="$(cat out/artifact_name.txt 2>/dev/null || true)"
if [[ -z "${ARTIFACT_BASENAME}" ]]; then
    ARTIFACT_BASENAME="unknown-Strawberry-${TARGET_PROFILE}-${TARGET_LTO}"
fi
ARTIFACT_ZIP_FILE="${ARTIFACT_BASENAME}.zip"

STAGING_DIR="$(mktemp -d)"
cp out/bzImage "${STAGING_DIR}/bzImage"
cp out/.config "${STAGING_DIR}/.config"

HEADERS_MANIFEST=(
    Makefile
    Module.symvers
    System.map
    include/config
    include/generated
    scripts
    arch/x86/include
    arch/x86/Makefile
    arch/x86/Makefile_32.cpu
    arch/x86/Kbuild
)

for entry in "${HEADERS_MANIFEST[@]}"; do
    if [[ -e "${entry}" ]]; then
        mkdir -p "${STAGING_DIR}/$(dirname "${entry}")"
        cp -r "${entry}" "${STAGING_DIR}/${entry}"
    fi
done

(
    cd "${STAGING_DIR}"
    zip -r "${OLDPWD}/${ARTIFACT_ZIP_FILE}" . -x '*.o' -x '*.cmd' -x '*.a'
)
rm -rf "${STAGING_DIR}"

printf '%s\n' "${ARTIFACT_ZIP_FILE}" > artifact_zip_name.txt
