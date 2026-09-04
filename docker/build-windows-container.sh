#!/usr/bin/env bash
set -euo pipefail

: "${XEMU_LOCAL_VERSION:?}"
: "${XEMU_ARCH:?}"
: "${XEMU_CONFIGURATION:?}"

apt-get update
apt-get install -qy curl ca-certificates unzip git patch
if [[ "$XEMU_ARCH" == "arm64" ]]; then
  apt-get install -qy zstd
fi

rm -rf /work
mkdir -p /work/extract /out
unzip -q /input/source.zip -d /work/extract
SRC=/work/extract
if [[ ! -f "$SRC/build.sh" ]]; then
  SRC="$(find /work/extract -mindepth 1 -maxdepth 1 -type d | head -n1)"
fi
[[ -f "$SRC/build.sh" ]] || { echo 'Could not locate Xemu build.sh after ZIP extraction' >&2; exit 1; }

bash /builder/common/apply-fix-overlay.sh "$SRC" /builder/FIX
bash /builder/common/set-debug-tools-profile.sh "$SRC" "${XEMU_DEBUG_TOOLS_PROFILE:-}"

cd "$SRC"
printf '%s' "$XEMU_LOCAL_VERSION" > XEMU_VERSION

export CCACHE_DIR=/tmp/xemu-ccache
export CCACHE_MAXSIZE=512M
export LTO_CACHE_DIR=/tmp/xemu-lto-cache
mkdir -p "$CCACHE_DIR"
ccache -z || true

if [[ "$XEMU_ARCH" == "arm64" ]]; then
  export CROSSPREFIX=aarch64-w64-mingw32.static-
  export CROSSAR=aarch64-w64-mingw32.static-ar
else
  export CROSSPREFIX=x86_64-w64-mingw32.static-
  export CROSSAR=x86_64-w64-mingw32.static-gcc-ar
fi

opts=()
if [[ "$XEMU_CONFIGURATION" == "debug" ]]; then
  opts+=(--debug)
elif [[ "$XEMU_ARCH" == "x86_64" ]]; then
  opts+=(--extra-cflags="-flto-incremental=${LTO_CACHE_DIR} -flto-partition=cache")
  opts+=(-Db_lto=true -Dx86_version=3)
  mkdir -p "$LTO_CACHE_DIR"
fi

./build.sh -p win64-cross "${opts[@]}"
ccache -s || true
mkdir -p "/out/dist"
cp -a dist/. /out/dist/
printf '%s\n' "$XEMU_LOCAL_VERSION" > /out/XEMU_VERSION.txt
if [[ -n "${XEMU_DEBUG_TOOLS_PROFILE:-}" ]]; then
  printf '%s\n' "$XEMU_DEBUG_TOOLS_PROFILE" > /out/DEBUG_TOOLS_PROFILE.txt
fi
