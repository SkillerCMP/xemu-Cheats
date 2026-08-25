#!/usr/bin/env bash
# Xemu Debug Tools - prepare the external libraries owned by this addition.
#
# This script intentionally lives under ui/xui/debug-tools so upstream xemu's
# build.sh does not need Capstone/Keystone bootstrap logic. It reuses/builds the
# pinned target libraries and emits a shell environment file for the caller.

set -euo pipefail

platform=""
arch=""
env_file=""
macos_sdk=""
macos_min_version=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) platform="$2"; shift 2 ;;
    --arch) arch="$2"; shift 2 ;;
    --env-file) env_file="$2"; shift 2 ;;
    --macos-sdk) macos_sdk="$2"; shift 2 ;;
    --macos-min-version) macos_min_version="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$platform" ]] || platform="$(uname -s)"
[[ -n "$arch" ]] || arch="$(uname -m)"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
capstone_env="$(mktemp "${TMPDIR:-/tmp}/xemu-debug-tools-capstone-env.XXXXXX")"
keystone_env="$(mktemp "${TMPDIR:-/tmp}/xemu-debug-tools-keystone-env.XXXXXX")"
cleanup() { rm -f "$capstone_env" "$keystone_env"; }
trap cleanup EXIT

log() { printf '[Debug Tools / Dependencies] %s\n' "$*" >&2; }

common_args=(--platform "$platform" --arch "$arch")
if [[ "$platform" == "Darwin" ]]; then
  if [[ -n "$macos_sdk" ]]; then common_args+=(--macos-sdk "$macos_sdk"); fi
  if [[ -n "$macos_min_version" ]]; then common_args+=(--macos-min-version "$macos_min_version"); fi
fi

log "preparing Capstone for ${platform}/${arch}"
bash "${script_dir}/build-capstone.sh" "${common_args[@]}" --env-file "$capstone_env"
# The helper emits only shell-escaped assignments owned by Debug Tools.
# shellcheck disable=SC1090
source "$capstone_env"

base_pkg_config_path="${PKG_CONFIG_PATH:-}"
if [[ -n "${XEMU_CAPSTONE_PKG_CONFIG_PATH:-}" ]]; then
  export PKG_CONFIG_PATH="${XEMU_CAPSTONE_PKG_CONFIG_PATH}${base_pkg_config_path:+:${base_pkg_config_path}}"
fi

log "preparing Keystone for ${platform}/${arch}"
bash "${script_dir}/build-keystone.sh" "${common_args[@]}" --env-file "$keystone_env"
# shellcheck disable=SC1090
source "$keystone_env"

combined_pkg_config_path=""
append_pkg_path() {
  local value="$1"
  [[ -n "$value" ]] || return 0
  if [[ -n "$combined_pkg_config_path" ]]; then
    combined_pkg_config_path="${combined_pkg_config_path}:${value}"
  else
    combined_pkg_config_path="$value"
  fi
}
append_pkg_path "${XEMU_KEYSTONE_PKG_CONFIG_PATH:-}"
append_pkg_path "${XEMU_CAPSTONE_PKG_CONFIG_PATH:-}"
append_pkg_path "$base_pkg_config_path"

export PKG_CONFIG_PATH="$combined_pkg_config_path"
export XEMU_DEBUG_TOOLS_DEPENDENCIES_READY=1

if [[ -n "$env_file" ]]; then
  {
    printf 'XEMU_CAPSTONE_PKG_CONFIG_PATH=%q\n' "${XEMU_CAPSTONE_PKG_CONFIG_PATH:-}"
    printf 'XEMU_CAPSTONE_TARGET_KIND=%q\n' "${XEMU_CAPSTONE_TARGET_KIND:-}"
    printf 'XEMU_CAPSTONE_TARGET_ARCH=%q\n' "${XEMU_CAPSTONE_TARGET_ARCH:-}"
    printf 'XEMU_KEYSTONE_PKG_CONFIG_PATH=%q\n' "${XEMU_KEYSTONE_PKG_CONFIG_PATH:-}"
    printf 'XEMU_KEYSTONE_TARGET_KIND=%q\n' "${XEMU_KEYSTONE_TARGET_KIND:-}"
    printf 'XEMU_KEYSTONE_TARGET_ARCH=%q\n' "${XEMU_KEYSTONE_TARGET_ARCH:-}"
    printf 'PKG_CONFIG_PATH=%q\n' "$PKG_CONFIG_PATH"
    printf 'XEMU_DEBUG_TOOLS_DEPENDENCIES_READY=%q\n' "$XEMU_DEBUG_TOOLS_DEPENDENCIES_READY"
  } > "$env_file"
fi

log "Capstone + Keystone environment ready"
