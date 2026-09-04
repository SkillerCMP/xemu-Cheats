#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 5 ]]; then
  echo "Usage: $0 <source.zip> <output-dir> <version> <arch-csv> <configuration-csv> [fix-folder] [debug-tools-profile]" >&2
  exit 2
fi

SOURCE_ZIP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$2"
VERSION="$3"
IFS=',' read -r -a ARCHES <<< "$4"
IFS=',' read -r -a CONFIGS <<< "$5"
FIX_ROOT="${6:-}"
DEBUG_TOOLS_PROFILE="${7:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIX_APPLIER="$SCRIPT_DIR/../common/apply-fix-overlay.sh"
PROFILE_APPLIER="$SCRIPT_DIR/../common/set-debug-tools-profile.sh"
if [[ -n "$DEBUG_TOOLS_PROFILE" ]]; then
  export XEMU_DEBUG_TOOLS_PROFILE="$DEBUG_TOOLS_PROFILE"
fi

if [[ "$(uname -s)" != Darwin ]]; then
  echo 'This script must run on a real macOS host.' >&2
  exit 1
fi

command -v brew >/dev/null || { echo 'Homebrew is required.' >&2; exit 1; }
command -v xcode-select >/dev/null || { echo 'Xcode Command Line Tools are required.' >&2; exit 1; }

export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
brew install ccache coreutils dylibbundler python@3.12
export PATH="$(brew --prefix python@3.12)/bin:$PATH"
python3.12 -m pip install --user pyyaml requests || true

WORK="$(mktemp -d /tmp/xemu-local-mac.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

for config in "${CONFIGS[@]}"; do
  built_archives=()
  for arch in "${ARCHES[@]}"; do
    TARGET="$OUT/$arch/$config"
    rm -rf "$WORK/src"
    mkdir -p "$WORK/src" "$TARGET"
    ditto -x -k "$SOURCE_ZIP" "$WORK/src"
    ROOT="$WORK/src"
    if [[ ! -f "$ROOT/build.sh" ]]; then
      ROOT="$(find "$WORK/src" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    fi
    [[ -f "$ROOT/build.sh" ]] || { echo 'Could not locate Xemu build.sh after ZIP extraction' >&2; exit 1; }
    if [[ -n "$FIX_ROOT" && -f "$FIX_APPLIER" ]]; then
      bash "$FIX_APPLIER" "$ROOT" "$FIX_ROOT"
    fi
    if [[ -f "$PROFILE_APPLIER" ]]; then
      bash "$PROFILE_APPLIER" "$ROOT" "$DEBUG_TOOLS_PROFILE"
    fi
    cd "$ROOT"
    printf '%s' "$VERSION" > XEMU_VERSION

    export CCACHE_DIR="$WORK/ccache-$arch-$config"
    export CCACHE_MAXSIZE=512M
    export LTO_CACHE_DIR="$WORK/lto-$arch-$config"
    mkdir -p "$CCACHE_DIR"
    export PATH="$(brew --prefix ccache)/libexec:$PATH"
    ccache -z || true

    opts=()
    if [[ "$config" == debug ]]; then
      opts+=(--debug)
    else
      opts+=(-Db_lto=true -Db_lto_mode=thin -Db_thinlto_cache=true -Db_thinlto_cache_dir="$LTO_CACHE_DIR" -Dx86_version=3)
      mkdir -p "$LTO_CACHE_DIR"
    fi

    ./build.sh -a "$arch" "${opts[@]}"
    ccache -s || true
    suffix=''
    [[ "$config" == debug ]] && suffix='-dbg'
    pkg="xemu-${VERSION}${suffix}-macos-${arch}-unsigned.zip"
    (cd dist && zip -r "$TARGET/$pkg" .)
    built_archives+=("$TARGET/$pkg")
    if [[ -n "$DEBUG_TOOLS_PROFILE" ]]; then
      printf '%s\n' "$DEBUG_TOOLS_PROFILE" > "$TARGET/DEBUG_TOOLS_PROFILE.txt"
    fi
  done

  # Match build-macos.yml's universal bundle stage.
  if [[ ${#built_archives[@]} -ge 2 ]]; then
    UWORK="$WORK/universal-$config"
    rm -rf "$UWORK"
    mkdir -p "$UWORK/dist"
    for arch in "${ARCHES[@]}"; do
      archive="$(find "$OUT/$arch/$config" -maxdepth 1 -name "*-macos-${arch}-unsigned.zip" | head -n1)"
      [[ -n "$archive" ]] || continue
      # Match GitHub: unzip both architecture bundles into the same app tree so
      # architecture-specific Libraries/<arch> directories are retained.
      unzip -q -o "$archive" -d "$UWORK/dist"
      mv "$UWORK/dist/xemu.app/Contents/MacOS/xemu" "$UWORK/xemu_$arch"
    done
    if [[ -f "$UWORK/xemu_x86_64" && -f "$UWORK/xemu_arm64" ]]; then
      lipo -create -output "$UWORK/dist/xemu.app/Contents/MacOS/xemu" "$UWORK/xemu_x86_64" "$UWORK/xemu_arm64"
      codesign --force --deep --preserve-metadata=entitlements,requirements,flags,runtime --sign - "$UWORK/dist/xemu.app/Contents/MacOS/xemu"
      suffix=''
      [[ "$config" == debug ]] && suffix='-dbg'
      mkdir -p "$OUT/universal/$config"
      (cd "$UWORK/dist" && zip -r "$OUT/universal/$config/xemu-${VERSION}${suffix}-macos-universal-unsigned.zip" .)
    fi
  fi
done
