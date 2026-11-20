#!/usr/bin/env bash
# Launcher for New Pokemon Snap Recomp AppImage on Linux.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

DEFAULT_APPIMAGE="${SCRIPT_DIR}/New-Pokemon-Snap-Linux-v1.3.0-x86_64.AppImage"
if [[ ! -x "${DEFAULT_APPIMAGE}" ]]; then
  DEFAULT_APPIMAGE="${HOME}/Applications/New-Pokemon-Snap-Linux-v1.3.0-x86_64.AppImage"
fi

APPIMAGE="${SNAP_APPIMAGE:-${DEFAULT_APPIMAGE}}"
DEFAULT_ROMFS="${HOME}/Games/Pokemon Snap/romfs"
DEFAULT_SAVE_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/SnapRecomp/profiles/p1/save"
REPO_BUILD="${SNAP_REPO_BUILD:-${REPO_ROOT}/build}"

MAIN_ELF="${SNAP_MAIN_ELF:-${REPO_BUILD}/linux-aot/elf/main.elf}"
SDK_ELF="${SNAP_SDK_ELF:-${REPO_BUILD}/linux-aot/elf/sdk.elf}"
MAIN_LIB="${SNAP_MAIN_LIB:-${REPO_BUILD}/linux-aot-abi3/main/lib/libsnap_main.a}"
SDK_LIB="${SNAP_SDK_LIB:-${REPO_BUILD}/linux-aot-abi3/sdk/lib/libsnap_sdk.a}"

export SNAP_ROMFS="${SNAP_ROMFS:-${DEFAULT_ROMFS}}"
export SNAP_SAVE_ROOT="${SNAP_SAVE_ROOT:-${DEFAULT_SAVE_ROOT}}"
export NVN_CACHE_DIR="${NVN_CACHE_DIR:-${XDG_CACHE_HOME:-${HOME}/.cache}/SnapRecomp/nvn_cache}"

fail() {
  echo "Run-New-Pokemon-Snap error: $1" >&2
  exit 1
}

[[ -x "${APPIMAGE}" ]] || fail "AppImage not found or not executable: ${APPIMAGE} (set SNAP_APPIMAGE=/path/to/AppImage)"
[[ -d "${SNAP_ROMFS}" && -r "${SNAP_ROMFS}" && -x "${SNAP_ROMFS}" ]] || fail "ROMFS directory not found or not accessible: ${SNAP_ROMFS} (set SNAP_ROMFS=/path/to/romfs)"
[[ -f "${MAIN_ELF}" ]] || fail "main.elf not found: ${MAIN_ELF} (set SNAP_REPO_BUILD=/path/to/build)"
[[ -f "${SDK_ELF}" ]] || fail "sdk.elf not found: ${SDK_ELF} (set SNAP_REPO_BUILD=/path/to/build)"
[[ -f "${MAIN_LIB}" ]] || fail "libsnap_main.a not found: ${MAIN_LIB} (set SNAP_REPO_BUILD=/path/to/build)"
[[ -f "${SDK_LIB}" ]] || fail "libsnap_sdk.a not found: ${SDK_LIB} (set SNAP_REPO_BUILD=/path/to/build)"

exec "${APPIMAGE}" \
  --main "${MAIN_ELF}" \
  --sdk "${SDK_ELF}" \
  --main-lib "${MAIN_LIB}" \
  --sdk-lib "${SDK_LIB}" \
  --play \
  --fullscreen \
  --allow-guest-exec \
  "$@"
