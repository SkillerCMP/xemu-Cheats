#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <xemu-source-root> <fix-folder>" >&2
  exit 2
fi

SRC="$1"
FIX_ROOT="$2"

[[ -d "$SRC" ]] || { echo "FIX overlay: source root does not exist: $SRC" >&2; exit 1; }

applied=0
if [[ -d "$FIX_ROOT" ]]; then
  echo "FIX overlay: checking $FIX_ROOT"
else
  echo "FIX overlay: no FIX folder found; using source ZIP unchanged."
fi

# Apply top-level ZIP fix packs first, alphabetically. If multiple packs replace
# the same file, the later archive wins. A pack may contain paths such as
# hw/xbox/xid.c directly, or one enclosing source-like directory.
if [[ -d "$FIX_ROOT" ]]; then
while IFS= read -r archive; do
  [[ -n "$archive" ]] || continue
  applied=1
  echo "FIX overlay ZIP: $(basename "$archive")"
  TMP_FIX="$(mktemp -d "${TMPDIR:-/tmp}/xemu-fix.XXXXXX")"
  unzip -q -o "$archive" -d "$TMP_FIX"

  PAYLOAD="$TMP_FIX"
  shopt -s dotglob nullglob
  entries=("$TMP_FIX"/*)
  shopt -u dotglob nullglob
  if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then
    candidate="${entries[0]}"
    if [[ -e "$candidate/build.sh" || -d "$candidate/hw" || -d "$candidate/ui" || -d "$candidate/.github" ]]; then
      PAYLOAD="$candidate"
    fi
  fi

  cp -a "$PAYLOAD"/. "$SRC"/
  rm -rf "$TMP_FIX"
done < <(find "$FIX_ROOT" -maxdepth 1 -type f -iname '*.zip' -print | LC_ALL=C sort)

# Apply top-level source patches next, alphabetically. We prefer git apply
# because it understands normal Git patches (including mode/binary metadata).
# If git's stricter context matching rejects a text patch only because earlier
# fixes shifted its hunk location, fall back to patch(1) with fuzz disabled.
# That fallback allows line offsets but still requires every context line to
# match exactly. A real overlap/incompatibility therefore stops the build.
while IFS= read -r patch_file; do
  [[ -n "$patch_file" ]] || continue
  applied=1
  patch_name="$(basename "$patch_file")"
  echo "FIX source patch: $patch_name"

  GIT_LOG="$(mktemp "${TMPDIR:-/tmp}/xemu-git-apply.XXXXXX")"
  PATCH_LOG="$(mktemp "${TMPDIR:-/tmp}/xemu-patch-apply.XXXXXX")"
  patch_method=''

  if command -v git >/dev/null 2>&1 && \
     git -C "$SRC" apply --check --whitespace=nowarn "$patch_file" >"$GIT_LOG" 2>&1; then
    git -C "$SRC" apply --whitespace=nowarn "$patch_file"
    patch_method='git apply'
  elif command -v patch >/dev/null 2>&1 && \
       (cd "$SRC" && patch --dry-run -p1 -F 0 --batch --forward < "$patch_file") >"$PATCH_LOG" 2>&1; then
    (cd "$SRC" && patch -p1 -F 0 --batch --forward < "$patch_file")
    patch_method='offset-aware patch -F 0'
  else
    echo "FIX patch ERROR: $patch_name does not apply cleanly to the current source tree." >&2
    if [[ -s "$GIT_LOG" ]]; then
      echo "--- git apply check ---" >&2
      cat "$GIT_LOG" >&2
    fi
    if [[ -s "$PATCH_LOG" ]]; then
      echo "--- offset-aware patch check ---" >&2
      cat "$PATCH_LOG" >&2
    fi
    rm -f "$GIT_LOG" "$PATCH_LOG"
    echo "The build is stopped so an overlapping/incompatible fix cannot be hidden." >&2
    exit 1
  fi

  rm -f "$GIT_LOG" "$PATCH_LOG"
  echo "FIX patch applied: $patch_name ($patch_method)"
done < <(find "$FIX_ROOT" -maxdepth 1 -type f -iname '*.patch' -print | LC_ALL=C sort)

# Loose files are applied last and therefore have highest precedence. This
# makes quick one-file experiments easy without rebuilding a fix-pack ZIP.
while IFS= read -r -d '' item; do
  rel="${item#"$FIX_ROOT"/}"
  [[ "$rel" == '_README.txt' ]] && continue
  if [[ "$rel" != */* && ( "$rel" == *.zip || "$rel" == *.ZIP || "$rel" == *.patch || "$rel" == *.PATCH ) ]]; then
    continue
  fi

  applied=1
  dest="$SRC/$rel"
  mkdir -p "$(dirname "$dest")"
  if [[ -L "$item" ]]; then
    rm -f "$dest"
    cp -a "$item" "$dest"
  else
    cp -a "$item" "$dest"
  fi
  echo "FIX overlay file: $rel"
done < <(find "$FIX_ROOT" -mindepth 1 \( -type f -o -type l \) -print0)
fi

if [[ "$applied" -eq 0 ]]; then
  echo "FIX overlay: FIX folder is empty; using source ZIP unchanged."
else
  echo "FIX overlay: complete."
fi

# ZIP overlays created on Windows or by generic ZIP writers can lose Unix
# executable mode bits.  Xemu invokes build.sh/configure directly, and several
# helper shell scripts are also expected to be executable.  Normalize these
# permissions after every overlay so a replacement file cannot turn a valid
# source tree into an exit-126 build failure.
[[ -f "$SRC/build.sh" ]] && chmod +x "$SRC/build.sh"
[[ -f "$SRC/configure" ]] && chmod +x "$SRC/configure"
find "$SRC" -type f -name '*.sh' -exec chmod +x {} +
echo "FIX overlay: normalized Xemu shell-script executable permissions."
