#!/usr/bin/env bash

set -e

TARGET_PROFILE="$1"
TARGET_LTO="$2"

# ---------------------------------------------------------------------------
# ccache: incremental rebuilds across CI runs. Everything that did not change
# since the last run is served from the ccache snapshot restored by the
# workflow; only edited files get recompiled.
# ---------------------------------------------------------------------------
export CCACHE_DIR="${CCACHE_DIR:-${HOME}/.cache/ccache}"
export CCACHE_COMPRESS=true
export CCACHE_COMPRESSLEVEL=6
export CCACHE_MAXSIZE=1500M          # keeps both the runner disk and the 10 GiB Actions cache quota safe
export CCACHE_COMPILERCHECK=content  # apt reinstalls the compiler -> mtime-based check would invalidate everything
export CCACHE_NOHASHDIR=true         # build root is stable, but keep hashes portable
mkdir -p "${CCACHE_DIR}"

# Shim directories differ by distro: Ubuntu/Debian use /usr/lib/ccache,
# Arch uses /usr/lib/ccache/bin. Pick whichever actually has a clang shim;
# build.sh falls back to an explicit `CC="ccache clang"` if none exists.
if command -v ccache >/dev/null 2>&1; then
    for d in /usr/lib/ccache/bin /usr/lib/ccache /usr/local/lib/ccache /usr/lib64/ccache; do
        if [ -x "${d}/clang" ]; then
            export PATH="${d}:${PATH}"
            echo "[ccache] compiler shims: ${d}"
            break
        fi
    done
    echo "[ccache] $(ccache --version | head -n1)"
    echo "[ccache] clang resolves to: $(command -v clang || echo MISSING)"
else
    echo "[ccache] WARNING: ccache not installed - full rebuild"
fi

# Never let the cache starve the build of disk space (ThinLTO link is large).
_avail_kb="$(df -Pk "${CCACHE_DIR}" | awk 'NR==2 {print $4}')"
if [ -n "${_avail_kb}" ]; then
    if [ "${_avail_kb}" -lt 3145728 ]; then        # < 3 GiB free: do not cache at all
        export CCACHE_DISABLE=1
        echo "[ccache] only ${_avail_kb} KiB free -> caching disabled for this run"
    elif [ "${_avail_kb}" -lt 8388608 ]; then      # < 8 GiB free: shrink the cache
        export CCACHE_MAXSIZE=512M
        echo "[ccache] only ${_avail_kb} KiB free -> maxsize 512M"
    fi
fi

show_ccache_stats() {
    echo "[ccache] ---- stats (build exit ${1}) ----"
    ccache -s 2>/dev/null | sed 's/^/[ccache] /' || true
    df -h "${CCACHE_DIR}" / 2>/dev/null | sed 's/^/[disk] /' || true
}
trap 'rc=$?; show_ccache_stats "$rc"' EXIT

echo "[ccache] ---- stats before build ----"
ccache -s 2>/dev/null | sed 's/^/[ccache] /' || true

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
