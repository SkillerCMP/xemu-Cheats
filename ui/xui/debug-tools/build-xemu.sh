#!/usr/bin/env bash
# Xemu Debug Tools build entry point.
#
# Prepares Debug Tools-owned Capstone/Keystone dependencies, then hands the
# unchanged argument list back to xemu's root build.sh. Root build.sh contains
# only a one-line redirect into this wrapper; all dependency-specific bootstrap
# code remains owned by Debug Tools.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
project_root="$(cd "${script_dir}/../../.." >/dev/null 2>&1 && pwd)"
platform="$(uname -s)"
target_arch="$(uname -m)"
args=("$@")

# Mirror only build.sh's leading -p/-a parsing so dependency preparation targets
# the same executable architecture. All arguments are still passed through to
# the real build.sh unchanged.
i=0
while (( i < ${#args[@]} )); do
  arg="${args[$i]}"
  case "$arg" in
    -j*|--debug)
      ((i += 1))
      ;;
    -p*)
      if (( i + 1 >= ${#args[@]} )); then
        echo "Debug Tools build wrapper: $arg requires a platform argument" >&2
        exit 2
      fi
      platform="${args[$((i + 1))]}"
      ((i += 2))
      ;;
    -a*)
      if (( i + 1 >= ${#args[@]} )); then
        echo "Debug Tools build wrapper: $arg requires an architecture argument" >&2
        exit 2
      fi
      target_arch="${args[$((i + 1))]}"
      ((i += 2))
      ;;
    *)
      break
      ;;
  esac
done

macos_sdk=""
macos_min_version=""
if [[ "$platform" == "Darwin" ]]; then
  case "$target_arch" in
    arm64) macos_min_version="14.0" ;;
    x86_64) macos_min_version="12.7.5" ;;
    *)
      echo "Debug Tools build wrapper: unsupported macOS arch $target_arch" >&2
      exit 1
      ;;
  esac

  # Prefer the same CommandLineTools SDK family used by xemu's build.sh. The
  # root build script will perform its own authoritative SDK validation later.
  sdk_base=/Library/Developer/CommandLineTools/SDKs
  if [[ -d "$sdk_base" ]]; then
    while IFS= read -r candidate; do
      [[ -d "$candidate" ]] || continue
      macos_sdk="$candidate"
      break
    done < <(printf '%s\n' "$sdk_base"/MacOSX*.sdk | sort -Vr)
  fi
fi

if [[ "${XEMU_DEBUG_TOOLS_SKIP_DEPENDENCY_BOOTSTRAP:-0}" != "1" ]]; then
  env_file="$(mktemp "${TMPDIR:-/tmp}/xemu-debug-tools-build-env.XXXXXX")"
  prepare_args=(--platform "$platform" --arch "$target_arch" --env-file "$env_file")
  if [[ -n "$macos_sdk" ]]; then prepare_args+=(--macos-sdk "$macos_sdk"); fi
  if [[ -n "$macos_min_version" ]]; then prepare_args+=(--macos-min-version "$macos_min_version"); fi

  bash "${script_dir}/prepare-build-dependencies.sh" "${prepare_args[@]}"
  # shellcheck disable=SC1090
  source "$env_file"
  rm -f "$env_file"
  export PKG_CONFIG_PATH XEMU_DEBUG_TOOLS_DEPENDENCIES_READY
fi

# Capstone is an optional upstream xemu dependency, but it is required by the
# Debug Tools main addition. Enable it without putting Capstone-specific logic in root build.sh.
has_capstone=0
for arg in "${args[@]}"; do
  if [[ "$arg" == "--enable-capstone" ]]; then
    has_capstone=1
    break
  fi
done
if [[ "$has_capstone" == "0" ]]; then
  args+=(--enable-capstone)
fi

printf '[Debug Tools / Build] profile=%s platform=%s arch=%s\n' \
  "${XEMU_DEBUG_TOOLS_PROFILE:-source-default}" "$platform" "$target_arch" >&2
printf '[Debug Tools / Build] entering xemu build.sh through one-line Debug Tools hook\n' >&2

# Prevent root build.sh's one-line integration hook from re-entering this
# wrapper. The dependency environment and adjusted argument list survive exec.
export XEMU_DEBUG_TOOLS_BUILD_WRAPPED=1

cd "$project_root"
exec bash "$project_root/build.sh" "${args[@]}"
