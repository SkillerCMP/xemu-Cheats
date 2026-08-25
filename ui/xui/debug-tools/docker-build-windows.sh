#!/usr/bin/env bash
set -euo pipefail

SOURCE=/input
OUT=/output
WORK=/tmp/xemu-github-windows-exact
TOOLCHAIN_LABEL='ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd'

rm -rf "$WORK"
mkdir -p "$WORK/source" "$WORK/buildsrc" "$OUT/logs" "$OUT/dist" "$OUT/source"

# Match build-windows.yml's explicit "Install build helpers" step.
# The pinned toolchain image intentionally does not include curl/cmake.
# GitHub installs them before the Windows build; our combined local Docker run
# also needs curl earlier because it performs the source-package job here too.
export DEBIAN_FRONTEND=noninteractive
{
  echo "Installing GitHub build helpers: curl cmake"
  apt-get update
  apt-get install -qy curl cmake
} 2>&1 | tee "$OUT/logs/install-build-helpers.txt"

# Work on Linux filesystem just like GitHub, never in the Windows bind mount.
cp -a "$SOURCE"/. "$WORK/source"/
cd "$WORK/source"
rm -rf build dist .xemu-native-build-logs
find . -type d -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
find . -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete 2>/dev/null || true

{
  echo "Toolchain image: $TOOLCHAIN_LABEL"
  echo "Build host: $(uname -a)"
  echo "gcc: $(x86_64-w64-mingw32.static-gcc --version | head -1)"
  echo "python: $(python3 --version 2>&1)"
  echo "meson: $(meson --version 2>&1)"
} > "$OUT/logs/environment.txt"

# Match the source job's validation before packaging.
python3 ./ui/xui/debug-tools/validate-project-layout.py --root . \
  2>&1 | tee "$OUT/logs/source-layout-validation.txt"
TEST_DIR=./ui/xui/debug-tools/tests
if [[ -d "$TEST_DIR" ]]; then
  TEST_RUNNER="$TEST_DIR/v287-run-regression-tests.py"
  if [[ ! -f "$TEST_RUNNER" ]]; then
    TEST_RUNNER="$TEST_DIR/run-regression-tests.py"
  fi
  if [[ ! -f "$TEST_RUNNER" ]]; then
    echo "ERROR: Debug Tools tests directory is present but no regression runner was found." \
      | tee "$OUT/logs/source-regression-tests.txt" >&2
    exit 25
  fi
  echo "Debug Tools tests detected; running regression suite." \
    | tee "$OUT/logs/source-regression-tests.txt"
  python3 "$TEST_RUNNER" --root . \
    2>&1 | tee -a "$OUT/logs/source-regression-tests.txt"
else
  echo "Debug Tools tests directory is not present; optional regression tests skipped." \
    | tee "$OUT/logs/source-regression-tests.txt"
fi

python3 ./ui/xui/debug-tools/restore-executable-bits.py --root . --update-git-index \
  2>&1 | tee "$OUT/logs/restore-executable-bits.txt"

# Make the disposable packaged source self-describe the profile that this run
# actually builds. This changes only the Linux working copy inside Docker; the
# user's source-tree build-profile.txt remains untouched. The fresh extraction
# used for the Windows build therefore defaults to the same profile even without
# the environment override.
if [[ -n "${XEMU_DEBUG_TOOLS_PROFILE:-}" ]]; then
  printf '%s\n' "$XEMU_DEBUG_TOOLS_PROFILE" > ./ui/xui/debug-tools/build-profile.txt
fi

# Local release ZIPs contain our new Debug Tools files as untracked relative to
# upstream xemu's embedded Git history. Ensure the source-owned shell entry
# points are executable in the Linux working copy, then stage this project
# subtree so archive-source.sh records those modes with the current contents.
chmod +x ./ui/xui/debug-tools/*.sh
git add -A ui/xui/debug-tools
# Local Windows entry points are part of the distributable fork but are
# untracked relative to upstream xemu. Stage them in this disposable source
# copy so the emitted source archive can rebuild the exact packaged fork.
for helper in Build-Xemu-DOCKER.bat Build-Xemu-MINGW64.bat; do
  if [[ -f "$helper" ]]; then
    git add -- "$helper"
  fi
done

if TAG=$(git describe --tags --match 'v*' 2>/dev/null); then
  PKG_VERSION=${TAG#v}
else
  SHORT_HASH=$(git rev-parse --short HEAD)
  PKG_VERSION="0.0.0-0-unofficial-${SHORT_HASH}"
fi
COMMIT=$(git rev-parse HEAD)
printf '%s' "$PKG_VERSION" > "$OUT/PACKAGE_VERSION.txt"
printf '%s' "$COMMIT" > "$OUT/XEMU_COMMIT.txt"

# Match build.yml: create the same source package, add XEMU_COMMIT/XEMU_VERSION,
# compress with zstd, then build from a fresh extraction of that package.
PKG_NAME="xemu-${PKG_VERSION}"
TAR_FILE="$WORK/${PKG_NAME}.tar"
TAR_PREFIX="./${PKG_NAME}/"
bash ./scripts/archive-source.sh "$TAR_FILE" "$TAR_PREFIX" \
  2>&1 | tee "$OUT/logs/archive-source.txt"

printf '%s' "$COMMIT" > XEMU_COMMIT
printf '%s' "$PKG_VERSION" > XEMU_VERSION
tar -r --file "$TAR_FILE" \
  --transform "s,^./,${TAR_PREFIX}," ./XEMU_COMMIT ./XEMU_VERSION
zstd -f "$TAR_FILE" -o "${TAR_FILE}.zst" \
  2>&1 | tee "$OUT/logs/zstd-source.txt"

# Keep the exact GitHub-style source archive and also emit a convenient ZIP
# beside the Windows build. The ZIP is produced from the same freshly packaged
# source tree that the Windows build consumes, not from the host working tree.
cp -f "${TAR_FILE}.zst" "$OUT/source/${PKG_NAME}.tar.zst"
tar -xf "${TAR_FILE}.zst" -C "$WORK/buildsrc" --strip-components=2
python3 - "$WORK/buildsrc" "$OUT/source/${PKG_NAME}-source.zip" "$PKG_NAME" <<'PY_SOURCE_ZIP'
import pathlib
import sys
import zipfile

source_root = pathlib.Path(sys.argv[1])
out_zip = pathlib.Path(sys.argv[2])
prefix = sys.argv[3]

with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
    for path in sorted(source_root.rglob("*")):
        if path.is_file():
            rel = path.relative_to(source_root).as_posix()
            zf.write(path, f"{prefix}/{rel}")
PY_SOURCE_ZIP
printf '%s\n' "${XEMU_DEBUG_TOOLS_PROFILE:-source-default}" > "$OUT/source/DEBUG_TOOLS_PROFILE.txt"
sha256sum "$OUT/source/${PKG_NAME}-source.zip" > "$OUT/logs/source-zip.sha256"
sha256sum "$OUT/source/${PKG_NAME}.tar.zst" > "$OUT/logs/source-tar-zst.sha256"

cd "$WORK/buildsrc"

# Match build-windows.yml.
python3 ./ui/xui/debug-tools/validate-project-layout.py --root . \
  2>&1 | tee "$OUT/logs/windows-layout-validation.txt"

export CCACHE_DIR=/tmp/xemu-ccache
export CCACHE_MAXSIZE=512M
export LTO_CACHE_DIR=/tmp/xemu-lto-ccache
export CROSSPREFIX=x86_64-w64-mingw32.static-
mkdir -p "$CCACHE_DIR" "$LTO_CACHE_DIR"
ccache -z >/dev/null 2>&1 || true

opts=(
  "--extra-cflags=-flto-incremental=${LTO_CACHE_DIR} -flto-partition=cache"
  -Db_lto=true
  -Dx86_version=3
)

# Enter xemu through its normal root build.sh. Its single Debug Tools hook line
# redirects once into build-xemu.sh, which prepares Capstone/Keystone and then
# returns to the normal xemu build path with XEMU_DEBUG_TOOLS_BUILD_WRAPPED=1.
set +e
bash ./build.sh -p win64-cross "${opts[@]}" \
  2>&1 | tee "$OUT/logs/build-windows.txt"
build_rc=${PIPESTATUS[0]}
set -e
ccache -s > "$OUT/logs/ccache-stats.txt" 2>&1 || true
if [[ "$build_rc" -ne 0 ]]; then
  echo "Build failed with exit code $build_rc" >&2
  exit "$build_rc"
fi

rm -rf "$OUT/dist"
mkdir -p "$OUT/dist"
cp -a dist/. "$OUT/dist/"

# Record the pre-cv2pdb executable so local results can be audited.
sha256sum "$OUT/dist/xemu.exe" > "$OUT/logs/xemu-pre-cv2pdb.sha256"
stat -c '%n %s bytes' "$OUT/dist/xemu.exe" > "$OUT/logs/xemu-pre-cv2pdb-size.txt"

echo "GitHub-equivalent cross build complete: $PKG_VERSION"
