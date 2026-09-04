#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <xemu-source-root>" >&2
  exit 2
fi

SRC="$1"
TARGET="$SRC/ui/xui/debug-tools/addons/memory-tools/memory-tools-dump.cc"

[[ -d "$SRC" ]] || { echo "Linux compatibility: source root does not exist: $SRC" >&2; exit 1; }

# Clang 21 + Debian hardening enables -Werror=format-security.  Some Debug
# Tools overlay revisions used a generic printf helper that instantiated
# fprintf(fp, format) for calls with no format arguments.  That is valid at
# runtime but is rejected by Clang as a non-literal format string.  Apply the
# narrow source fix after all user FIX overlays so a newer/older replacement
# ZIP cannot accidentally reintroduce the known Linux-only build break.
if [[ ! -f "$TARGET" ]]; then
  echo "Linux compatibility: Memory Tools dump source not present; no format-security fix needed."
  exit 0
fi

if grep -Fq 'if constexpr (sizeof...(args) == 0)' "$TARGET"; then
  echo "Linux compatibility: Memory Tools format-security helper already compatible."
  exit 0
fi

python3 - "$TARGET" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = """        if (ok && std::fprintf(fp, format, args...) < 0) {\n            ok = false;\n        }\n"""
new = """        if (!ok) {\n            return;\n        }\n        if constexpr (sizeof...(args) == 0) {\n            if (std::fputs(format, fp) == EOF) {\n                ok = false;\n            }\n        } else if (std::fprintf(fp, format, args...) < 0) {\n            ok = false;\n        }\n"""

count = text.count(old)
if count == 1:
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("Linux compatibility: repaired Memory Tools Clang 21 format-security helper.")
    raise SystemExit(0)

# Do not guess if a future overlay changes this function in an unknown way.
# If the vulnerable call is still present, stop with a diagnostic instead of
# silently proceeding to another 2000-file compile that will fail at the end.
if "std::fprintf(fp, format, args...)" in text:
    print(
        "Linux compatibility ERROR: Memory Tools contains the known format-security call "
        "but its surrounding source no longer matches the supported repair pattern.",
        file=sys.stderr,
    )
    raise SystemExit(1)

print("Linux compatibility: Memory Tools source does not contain the known vulnerable helper; no change needed.")
PY

if grep -Fq 'if constexpr (sizeof...(args) == 0)' "$TARGET"; then
  echo "Linux compatibility: format-security guard verified."
fi
