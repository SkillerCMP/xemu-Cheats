#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <xemu-source-root> [debug-tools-profile]" >&2
  exit 2
fi

SRC="$1"
PROFILE="${2:-${XEMU_DEBUG_TOOLS_PROFILE:-}}"

if [[ -z "$PROFILE" ]]; then
  exit 0
fi

case "$PROFILE" in
  main|main+hdd|main+memory|full) ;;
  *)
    echo "Invalid Debug Tools profile: $PROFILE" >&2
    echo "Expected one of: main, main+hdd, main+memory, full" >&2
    exit 2
    ;;
esac

PROFILE_FILE="$SRC/ui/xui/debug-tools/build-profile.txt"
if [[ ! -f "$PROFILE_FILE" ]]; then
  echo "Debug Tools profile was selected, but the overlay did not provide:" >&2
  echo "  ui/xui/debug-tools/build-profile.txt" >&2
  exit 1
fi

printf '%s\n' "$PROFILE" > "$PROFILE_FILE"
echo "Debug Tools build profile: $PROFILE"
