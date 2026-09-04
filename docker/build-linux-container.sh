#!/usr/bin/env bash
set -Eeuo pipefail

: "${XEMU_LOCAL_VERSION:?}"
: "${XEMU_ARCH:?}"
: "${XEMU_CONFIGURATION:?}"
LLVM_MAJOR="${LLVM_MAJOR:-21}"

export DEBIAN_FRONTEND=noninteractive
export CCACHE_DIR=/tmp/xemu-ccache
export CCACHE_MAXSIZE=512M
export LTO_CACHE_DIR=/tmp/xemu-lto-cache
export LTO_CACHE_MAXSIZE=512m
export DEB_BUILD_MAINT_OPTIONS=optimize=-lto
export DEB_CFLAGS_MAINT_APPEND='-gdwarf-4 -gstrict-dwarf'
export DEB_CXXFLAGS_MAINT_APPEND='-gdwarf-4 -gstrict-dwarf'
export DEB_LDFLAGS_MAINT_APPEND='-Wl,--build-id=sha1'

BUILD_STAGE='container startup'
SRC=''

fail() {
  echo "ERROR: $*" >&2
  return 1
}

preserve_failure_diagnostics() {
  local status="$1"
  local failed_command="$2"
  trap - ERR
  set +e

  mkdir -p /out/logs
  {
    echo "Linux build failed"
    echo "stage=$BUILD_STAGE"
    echo "exit_code=$status"
    echo "arch=$XEMU_ARCH"
    echo "configuration=$XEMU_CONFIGURATION"
    echo "llvm_major=$LLVM_MAJOR"
    echo "version=$XEMU_LOCAL_VERSION"
    echo "failed_command=$failed_command"
    date -R
  } > /out/logs/failure-summary.txt

  if [[ -n "$SRC" && -d "$SRC" ]]; then
    # Meson may be nested under Xemu's build directory. Preserve every useful
    # log we can find before Docker --rm removes the failed container.
    while IFS= read -r -d '' log; do
      rel="${log#"$SRC"/}"
      safe="${rel//\//__}"
      cp -a "$log" "/out/logs/${safe}" 2>/dev/null || true
    done < <(find "$SRC" -type f \( \
      -path '*/meson-logs/meson-log.txt' -o \
      -name '.ninja_log' -o \
      -name '.ninja_deps' -o \
      -name 'compile_commands.json' \
    \) -print0 2>/dev/null)
  fi

  command -v ccache >/dev/null 2>&1 && ccache -s > /out/logs/ccache-stats.txt 2>&1 || true
  dpkg -l > /out/logs/dpkg-packages.txt 2>&1 || true
  env | sort > /out/logs/environment.txt 2>&1 || true

  echo "ERROR: Linux build failed during '$BUILD_STAGE' (exit $status)." >&2
  echo "Failure diagnostics were copied to /out/logs." >&2
  exit "$status"
}
trap 'status=$?; failed_command=$BASH_COMMAND; preserve_failure_diagnostics "$status" "$failed_command"' ERR

BUILD_STAGE='base dependency installation'
apt-get -qy update
apt-get install -qy --no-install-recommends \
  ca-certificates curl wget gnupg unzip ccache libfuse2 libusb-1.0-0-dev \
  build-essential devscripts dpkg-dev fakeroot python3 python3-pip meson ninja-build \
  zstd file git patch xz-utils

BUILD_STAGE='LLVM repository setup'
install -d /etc/apt/keyrings
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /etc/apt/keyrings/llvm.gpg
cat >/etc/apt/sources.list.d/llvm.list <<EOF_LLVM
deb [signed-by=/etc/apt/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-${LLVM_MAJOR} main
deb-src [signed-by=/etc/apt/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-${LLVM_MAJOR} main
EOF_LLVM
apt-get -qy update

BUILD_STAGE='source extraction and FIX overlay'
rm -rf /work
mkdir -p /work/extract /out/logs
unzip -q /input/source.zip -d /work/extract
SRC=/work/extract
if [[ ! -f "$SRC/build.sh" ]]; then
  SRC="$(find /work/extract -mindepth 1 -maxdepth 1 -type d -print -quit)"
fi
[[ -f "$SRC/build.sh" ]] || fail 'Could not locate Xemu build.sh after ZIP extraction'

bash /builder/common/apply-fix-overlay.sh "$SRC" /builder/FIX
bash /builder/common/set-debug-tools-profile.sh "$SRC" "${XEMU_DEBUG_TOOLS_PROFILE:-}"

BUILD_STAGE='Linux source compatibility fixes'
bash /builder/common/apply-linux-compat-fixes.sh "$SRC"

BUILD_STAGE='Debian build dependency installation'
cd "$SRC"
printf '%s' "$XEMU_LOCAL_VERSION" > XEMU_VERSION
printf 'xemu (1:0.0.0-0) unstable; urgency=medium\n\n  * Built from %s\n\n -- Local Xemu Builder <local@localhost>  %s\n' \
  "$XEMU_LOCAL_VERSION" "$(date -R)" > debian/changelog
apt-get -qy build-dep .

# Match Xemu's current Linux workflow: obtain one internally consistent LLVM
# snapshot version directly from apt.llvm.org's package pool, then install all
# required LLVM packages from that exact version together. Installing the
# rolling packages individually through APT can fail while the snapshot
# metadata and package pool are briefly out of sync.
BUILD_STAGE="LLVM ${LLVM_MAJOR} package installation"
case "$XEMU_ARCH" in
  x86_64) LLVM_DEB_ARCH=amd64 ;;
  aarch64) LLVM_DEB_ARCH=arm64 ;;
  *) fail "Unsupported Linux LLVM package architecture: $XEMU_ARCH" ;;
esac
LLVM_POOL="https://apt.llvm.org/jammy/pool/main/l/llvm-toolchain-${LLVM_MAJOR}/"
LLVM_TMP="/tmp/llvm${LLVM_MAJOR}"
LLVM_HTML="$(curl -fsSL --compressed "$LLVM_POOL")"
LLVM_VER="$(grep -m1 -oE "libllvm${LLVM_MAJOR}_[0-9][^\"]+_${LLVM_DEB_ARCH}\\.deb" <<<"$LLVM_HTML" \
  | sed -E "s/^libllvm${LLVM_MAJOR}_//; s/_${LLVM_DEB_ARCH}\\.deb$//")"
[[ -n "$LLVM_VER" ]] || fail "Could not determine LLVM ${LLVM_MAJOR} pool version for ${LLVM_DEB_ARCH}"
echo "Using LLVM pool version: $LLVM_VER (${LLVM_DEB_ARCH})"
rm -rf "$LLVM_TMP"
mkdir -p "$LLVM_TMP"
cd "$LLVM_TMP"
llvm_fetch() {
  local pkg="$1"
  local file="${pkg}_${LLVM_VER}_${LLVM_DEB_ARCH}.deb"
  curl -fsSLO "${LLVM_POOL}${file}"
}
for pkg in \
  "clang-${LLVM_MAJOR}" "clang-tools-${LLVM_MAJOR}" "lld-${LLVM_MAJOR}" \
  "libllvm${LLVM_MAJOR}" "libclang-cpp${LLVM_MAJOR}" "libclang-common-${LLVM_MAJOR}-dev" \
  "libclang1-${LLVM_MAJOR}" "libclang-rt-${LLVM_MAJOR}-dev" \
  "llvm-${LLVM_MAJOR}" "llvm-${LLVM_MAJOR}-dev" "llvm-${LLVM_MAJOR}-linker-tools" \
  "llvm-${LLVM_MAJOR}-runtime" "llvm-${LLVM_MAJOR}-tools"
do
  llvm_fetch "$pkg"
done
apt-get install -y --no-install-recommends ./*.deb

BUILD_STAGE='compiler/cache setup'
export PATH=/usr/lib/ccache:$PATH
export CC="ccache clang-${LLVM_MAJOR}"
export CXX="ccache clang++-${LLVM_MAJOR}"
export CC_LD="lld-${LLVM_MAJOR}"
export CXX_LD="lld-${LLVM_MAJOR}"
export AR="llvm-ar-${LLVM_MAJOR}"
export RANLIB="llvm-ranlib-${LLVM_MAJOR}"
export NM="llvm-nm-${LLVM_MAJOR}"
mkdir -p "$CCACHE_DIR"
ccache -z || true

if [[ "$XEMU_CONFIGURATION" == "debug" ]]; then
  export XEMU_BUILD_OPTIONS='--debug'
else
  mkdir -p "$LTO_CACHE_DIR"
  export XEMU_BUILD_OPTIONS="--extra-ldflags=-Wl,--thinlto-cache-policy=cache_size_bytes=${LTO_CACHE_MAXSIZE} -Db_lto=true -Db_lto_mode=thin -Db_thinlto_cache=true -Db_thinlto_cache_dir=${LTO_CACHE_DIR} -Dx86_version=3"
fi

# Keep the GitHub workflow behavior: Debian package first.
# dpkg-genbuildinfo is intentionally bypassed there for speed.
BUILD_STAGE='Debian package compile'
cd "$SRC"
if [[ -x /usr/bin/dpkg-genbuildinfo ]]; then
  mv /usr/bin/dpkg-genbuildinfo /usr/bin/dpkg-genbuildinfo.real
  ln -s /bin/true /usr/bin/dpkg-genbuildinfo
fi

dpkg-buildpackage --no-sign -b
mkdir -p /out/deb
# dpkg-buildpackage writes binary packages to the parent directory of the
# source tree.  SRC may be /work/extract for root-level ZIPs or a nested
# directory such as /work/extract/xemu-master, so derive the package output
# directory from SRC instead of assuming /work.
PKG_OUT_DIR="$(dirname "$SRC")"
echo "Debian package output directory: $PKG_OUT_DIR"
find "$PKG_OUT_DIR" -maxdepth 1 \( -name '*.deb' -o -name '*.ddeb' \) -print -exec cp -a {} /out/deb/ \;
ccache -s | tee /out/logs/ccache-stats.txt || true

# Reproduce the AppImage bundling stage as closely as practical in Docker.
BUILD_STAGE='AppImage generation'
cd /work
wget --no-verbose "https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-${XEMU_ARCH}.AppImage" -O linuxdeploy.AppImage
chmod +x linuxdeploy.AppImage
rm -rf appimage arwork
mkdir -p appimage arwork
cd arwork
DEB_FILE="$(find /out/deb -maxdepth 1 -name '*.deb' -print -quit)"
if [[ -z "$DEB_FILE" ]]; then
  fail 'No .deb produced; cannot build AppImage'
fi
ar x "$DEB_FILE"
DATA_TAR="$(find . -maxdepth 1 -name 'data.tar*' -print -quit)"
tar -C ../appimage -xf "$DATA_TAR"
install -DT "$SRC/xemu.metainfo.xml" /work/appimage/usr/share/metainfo/xemu.metainfo.xml
cd /work
APPIMAGE_EXTRACT_AND_RUN=1 ./linuxdeploy.AppImage --output appimage --appdir appimage

BUILD_STAGE='artifact bundling'
mkdir -p /out/dist
find /work -maxdepth 1 -name 'xemu-*.AppImage' -exec cp -a {} /out/dist/ \;
tar -czf "/out/dist/xemu-ubuntu-${XEMU_ARCH}-${XEMU_CONFIGURATION}.tgz" -C /out deb
printf '%s\n' "$XEMU_LOCAL_VERSION" > /out/XEMU_VERSION.txt
if [[ -n "${XEMU_DEBUG_TOOLS_PROFILE:-}" ]]; then
  printf '%s\n' "$XEMU_DEBUG_TOOLS_PROFILE" > /out/DEBUG_TOOLS_PROFILE.txt
fi

BUILD_STAGE='complete'
printf 'Linux build completed successfully.\n' > /out/logs/build-success.txt
trap - ERR
