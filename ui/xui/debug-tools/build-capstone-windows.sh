#!/usr/bin/env bash
# Xemu RAW Cheat Engine / Debugger - Windows cross-build Capstone helper.
#
# Keep custom debugger dependency setup with the debugger implementation rather
# than embedding it in the upstream GitHub workflow.  The workflow intentionally
# calls this file with `bash`, so the source archive does not need to preserve
# the executable bit for this helper.

set -euxo pipefail

CAPSTONE_VERSION="${CAPSTONE_VERSION:-5.0.9}"
CAPSTONE_PREFIX="${CAPSTONE_PREFIX:-/usr/local/mxe/usr/x86_64-w64-mingw32.static}"
CROSSPREFIX="${CROSSPREFIX:-x86_64-w64-mingw32.static-}"

# Resolve the MXE tools to absolute paths. Do not hand CMake a bare gcc-ar or
# gcc-ranlib name: some CMake versions interpret it as a relative path. Capstone
# is not built with LTO here, so the normal MinGW ar/ranlib tools are sufficient.
CAPSTONE_CC="$(command -v "${CROSSPREFIX}gcc")"
CAPSTONE_AR="$(command -v "${CROSSPREFIX}ar")"
CAPSTONE_RANLIB="$(command -v "${CROSSPREFIX}ranlib")"
CAPSTONE_PKG_CONFIG="$(command -v "${CROSSPREFIX}pkg-config")"

test -x "${CAPSTONE_CC}"
test -x "${CAPSTONE_AR}"
test -x "${CAPSTONE_RANLIB}"
test -x "${CAPSTONE_PKG_CONFIG}"
printf 'Capstone CC: %s\nCapstone AR: %s\nCapstone RANLIB: %s\n' \
  "${CAPSTONE_CC}" "${CAPSTONE_AR}" "${CAPSTONE_RANLIB}"

rm -rf "/tmp/capstone-${CAPSTONE_VERSION}" /tmp/capstone-build
curl -fL --retry 3 --retry-delay 2 \
  "https://github.com/capstone-engine/capstone/archive/refs/tags/${CAPSTONE_VERSION}.tar.gz" \
  -o /tmp/capstone.tar.gz
tar -xzf /tmp/capstone.tar.gz -C /tmp

cmake -S "/tmp/capstone-${CAPSTONE_VERSION}" -B /tmp/capstone-build -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_C_COMPILER="${CAPSTONE_CC}" \
  -DCMAKE_AR="${CAPSTONE_AR}" \
  -DCMAKE_RANLIB="${CAPSTONE_RANLIB}" \
  -DCMAKE_INSTALL_PREFIX="${CAPSTONE_PREFIX}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_STATIC_LIBS=ON \
  -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF \
  -DCAPSTONE_X86_SUPPORT=ON \
  -DCAPSTONE_BUILD_TESTS=OFF \
  -DCAPSTONE_BUILD_CSTOOL=OFF \
  -DCAPSTONE_BUILD_CSTEST=OFF

cmake --build /tmp/capstone-build --parallel
cmake --install /tmp/capstone-build

# Prove that the same cross pkg-config wrapper Meson will use can see the
# installed library and that its advertised header flags compile successfully.
"${CAPSTONE_PKG_CONFIG}" --modversion capstone
CAPSTONE_CFLAGS="$("${CAPSTONE_PKG_CONFIG}" --cflags capstone)"
CAPSTONE_LIBS="$("${CAPSTONE_PKG_CONFIG}" --libs capstone)"
printf 'Capstone CFLAGS: %s\nCapstone LIBS: %s\n' \
  "${CAPSTONE_CFLAGS}" "${CAPSTONE_LIBS}"
printf '#include <capstone.h>\nint main(void) { csh h = 0; return (int)h; }\n' \
  > /tmp/capstone-header-smoke.c
# Intentional word splitting: pkg-config returns compiler flag tokens.
# shellcheck disable=SC2086
"${CAPSTONE_CC}" ${CAPSTONE_CFLAGS} -c /tmp/capstone-header-smoke.c \
  -o /tmp/capstone-header-smoke.o
test -s /tmp/capstone-header-smoke.o
