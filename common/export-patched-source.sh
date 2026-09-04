#!/usr/bin/env bash
set -euo pipefail

: "${XEMU_LOCAL_VERSION:?}"
: "${XEMU_PATCHED_SOURCE_NAME:?}"

apt-get update
apt-get install -qy --no-install-recommends unzip git patch zip ca-certificates

rm -rf /work/source-export
mkdir -p /work/source-export /source-out
unzip -q /input/source.zip -d /work/source-export
SRC=/work/source-export
if [[ ! -f "$SRC/build.sh" ]]; then
  SRC="$(find /work/source-export -mindepth 1 -maxdepth 1 -type d | head -n1)"
fi
[[ -f "$SRC/build.sh" ]] || { echo 'Could not locate Xemu build.sh after ZIP extraction' >&2; exit 1; }

bash /builder/common/apply-fix-overlay.sh "$SRC" /builder/FIX
bash /builder/common/set-debug-tools-profile.sh "$SRC" "${XEMU_DEBUG_TOOLS_PROFILE:-}"
printf '%s' "$XEMU_LOCAL_VERSION" > "$SRC/XEMU_VERSION"

ROOT_PARENT="$(dirname "$SRC")"
ROOT_NAME="$(basename "$SRC")"
OUT_FILE="/source-out/$XEMU_PATCHED_SOURCE_NAME"
rm -f "$OUT_FILE"
(
  cd "$ROOT_PARENT"
  zip -qry "$OUT_FILE" "$ROOT_NAME"
)

sha256sum "$OUT_FILE" > "$OUT_FILE.sha256"
printf 'Patched source root: %s\nVersion: %s\nDebug Tools profile: %s\n' \
  "$ROOT_NAME" "$XEMU_LOCAL_VERSION" "${XEMU_DEBUG_TOOLS_PROFILE:-not selected}" > /source-out/patched-source-info.txt

echo "Patched source ZIP: $OUT_FILE"
sha256sum "$OUT_FILE"
