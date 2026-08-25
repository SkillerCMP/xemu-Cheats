#!/usr/bin/env bash
# Xemu Debug Tools - target-aware Keystone bootstrap.
#
# Builds a static Keystone library containing only the X86 assembler backend for
# the host/target that is currently compiling xemu. F0 source is IA-32 even when
# the xemu executable itself targets ARM64.

set -euo pipefail

KEYSTONE_VERSION="${KEYSTONE_VERSION:-0.9.2}"
# Keystone 0.9.2's upstream CMake/pkg-config metadata reports only "0.9"
# (major/minor), even though the source release itself is 0.9.2. Keep the
# exact source release pinned independently by KEYSTONE_VERSION + SHA256.
KEYSTONE_PKGCONFIG_MIN_VERSION="${KEYSTONE_PKGCONFIG_MIN_VERSION:-0.9}"
KEYSTONE_SOURCE_URL="${KEYSTONE_SOURCE_URL:-https://files.pythonhosted.org/packages/0a/65/3a2e7e55cc1db188869bbbacee60036828330e0ce57fc5f05a3167ab4b4d/keystone-engine-${KEYSTONE_VERSION}.tar.gz}"
KEYSTONE_SOURCE_SHA256="${KEYSTONE_SOURCE_SHA256:-2f7af62dab0ce6c2732dbb4f31cfa2184a8a149e280b96b92ebc0db84c6e50f5}"

platform="${KEYSTONE_TARGET_PLATFORM:-}"
arch="${KEYSTONE_TARGET_ARCH:-}"
env_file=""
macos_sdk="${KEYSTONE_MACOS_SDK:-}"
macos_min_version="${KEYSTONE_MACOS_MIN_VERSION:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) platform="$2"; shift 2 ;;
    --arch) arch="$2"; shift 2 ;;
    --env-file) env_file="$2"; shift 2 ;;
    --macos-sdk) macos_sdk="$2"; shift 2 ;;
    --macos-min-version) macos_min_version="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$platform" ]]; then platform="$(uname -s)"; fi
if [[ -z "$arch" ]]; then arch="$(uname -m)"; fi

log() { printf '[Debug Tools / Keystone] %s\n' "$*" >&2; }
need_command() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Required command not found: $1" >&2
    exit 1
  }
}

compiler_from_spec() {
  local spec="$1"
  local first rest
  first="${spec%% *}"
  rest="${spec#"$first"}"
  rest="${rest# }"
  if [[ "$first" == "ccache" && -n "$rest" ]]; then
    KEYSTONE_CMAKE_LAUNCHER="ccache"
    printf '%s\n' "${rest%% *}"
  else
    printf '%s\n' "$first"
  fi
}

KEYSTONE_CMAKE_LAUNCHER=""
target_kind="native"
target_arch="$arch"
tool_prefix=""

case "$platform" in
  win64-cross)
    target_kind="windows-cross"
    tool_prefix="${CROSSPREFIX:-}"
    if [[ -z "$tool_prefix" ]]; then
      echo "win64-cross Keystone bootstrap requires CROSSPREFIX" >&2
      exit 1
    fi
    case "$(basename "${tool_prefix%-}")" in
      aarch64-*|arm64-*) target_arch="aarch64" ;;
      x86_64-*) target_arch="x86_64" ;;
    esac
    ;;
  CYGWIN*|MINGW*|MSYS*)
    target_kind="windows-native"
    ;;
  Darwin)
    target_kind="macos"
    ;;
  Linux)
    if [[ -n "${DEB_HOST_GNU_TYPE:-}" && -n "${DEB_BUILD_GNU_TYPE:-}" &&
          "${DEB_HOST_GNU_TYPE}" != "${DEB_BUILD_GNU_TYPE}" ]]; then
      target_kind="linux-cross"
      tool_prefix="${DEB_HOST_GNU_TYPE}-"
      case "${DEB_HOST_ARCH:-}" in
        arm64) target_arch="aarch64" ;;
        amd64) target_arch="x86_64" ;;
        *) target_arch="${DEB_HOST_ARCH:-$arch}" ;;
      esac
    fi
    ;;
  *)
    echo "Unsupported Keystone bootstrap platform: $platform" >&2
    exit 1
    ;;
esac

if [[ "$target_kind" == "windows-cross" || "$target_kind" == "linux-cross" ]]; then
  cc_name="${tool_prefix}gcc"
  cxx_name="${tool_prefix}g++"
  if ! command -v "$cc_name" >/dev/null 2>&1; then cc_name="${tool_prefix}clang"; fi
  if ! command -v "$cxx_name" >/dev/null 2>&1; then cxx_name="${tool_prefix}clang++"; fi
  KEYSTONE_CC="${KEYSTONE_CC:-$(command -v "$cc_name")}" 
  KEYSTONE_CXX="${KEYSTONE_CXX:-$(command -v "$cxx_name")}" 
  if [[ -n "${CROSSAR:-}" && "$target_kind" == "windows-cross" ]]; then
    KEYSTONE_AR="${KEYSTONE_AR:-$(command -v "${CROSSAR}")}" 
  else
    KEYSTONE_AR="${KEYSTONE_AR:-$(command -v "${tool_prefix}ar")}" 
  fi
  KEYSTONE_RANLIB="${KEYSTONE_RANLIB:-$(command -v "${tool_prefix}ranlib")}" 
  if command -v "${tool_prefix}pkg-config" >/dev/null 2>&1; then
    KEYSTONE_PKG_CONFIG="${KEYSTONE_PKG_CONFIG:-$(command -v "${tool_prefix}pkg-config")}" 
  else
    KEYSTONE_PKG_CONFIG="${KEYSTONE_PKG_CONFIG:-$(command -v pkg-config)}" 
  fi
else
  cc_spec="${KEYSTONE_CC:-${CC:-cc}}"
  cxx_spec="${KEYSTONE_CXX:-${CXX:-c++}}"
  KEYSTONE_CC="$(compiler_from_spec "$cc_spec")"
  KEYSTONE_CC="$(command -v "$KEYSTONE_CC")"
  KEYSTONE_CXX="$(compiler_from_spec "$cxx_spec")"
  KEYSTONE_CXX="$(command -v "$KEYSTONE_CXX")"
  KEYSTONE_AR="${KEYSTONE_AR:-${AR:-}}"
  KEYSTONE_RANLIB="${KEYSTONE_RANLIB:-${RANLIB:-}}"
  KEYSTONE_PKG_CONFIG="${KEYSTONE_PKG_CONFIG:-${PKG_CONFIG:-pkg-config}}"
  KEYSTONE_PKG_CONFIG="$(command -v "$KEYSTONE_PKG_CONFIG")"
  if [[ -n "$KEYSTONE_AR" ]]; then KEYSTONE_AR="$(command -v "$KEYSTONE_AR")"; fi
  if [[ -n "$KEYSTONE_RANLIB" ]]; then KEYSTONE_RANLIB="$(command -v "$KEYSTONE_RANLIB")"; fi
fi

need_command cmake
need_command curl
need_command tar
need_command ninja
need_command python3

case "$target_kind" in
  windows-cross)
    triplet="$(basename "${tool_prefix%-}")"
    KEYSTONE_PREFIX="${KEYSTONE_PREFIX:-/usr/local/mxe/usr/${triplet}}"
    ;;
  *)
    safe_platform="$(printf '%s' "$platform" | tr '/: ' '___')"
    KEYSTONE_PREFIX="${KEYSTONE_PREFIX:-${TMPDIR:-/tmp}/xemu-debug-tools-keystone/${safe_platform}-${target_arch}}"
    ;;
esac

pkg_paths=("${KEYSTONE_PREFIX}/lib/pkgconfig" "${KEYSTONE_PREFIX}/lib64/pkgconfig")
pkg_path_joined="${pkg_paths[0]}:${pkg_paths[1]}"
probe_pkg_path="$pkg_path_joined"
if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
  probe_pkg_path="${probe_pkg_path}:${PKG_CONFIG_PATH}"
fi

smoke_dir="${TMPDIR:-/tmp}/xemu-keystone-smoke-${$}"
mkdir -p "$smoke_dir"
trap 'rm -rf "$smoke_dir"' EXIT
smoke_output="${smoke_dir}/keystone-smoke"
case "$target_kind" in
  windows-cross|windows-native) smoke_output="${smoke_output}.exe" ;;
esac
cat > "${smoke_dir}/keystone.cc" <<'SMOKE'
#include <keystone/keystone.h>
int main(void) {
    ks_engine *ks = nullptr;
    if (ks_open(KS_ARCH_X86, KS_MODE_32, &ks) != KS_ERR_OK) return 1;
    ks_close(ks);
    return 0;
}
SMOKE

patch_windows_keystone_pkgconfig() {
  case "$target_kind" in
    windows-cross|windows-native) ;;
    *) return 0 ;;
  esac

  local pkg_dir pc patched=0
  for pkg_dir in "${pkg_paths[@]}"; do
    pc="${pkg_dir}/keystone.pc"
    [[ -f "$pc" ]] || continue
    python3 - "$pc" <<'PY_KEYSTONE_PC'
from pathlib import Path
import sys

pc = Path(sys.argv[1])
text = pc.read_text(encoding="utf-8")
required = ["-lshell32", "-lole32", "-luuid"]
lines = text.splitlines()

for index, line in enumerate(lines):
    if line.startswith("Libs.private:"):
        existing = line[len("Libs.private:"):].split()
        missing = [flag for flag in required if flag not in existing]
        if missing:
            lines[index] = line.rstrip() + " " + " ".join(missing)
        break
else:
    insert_at = len(lines)
    for index, line in enumerate(lines):
        if line.startswith("Libs:"):
            insert_at = index + 1
            break
    lines.insert(insert_at, "Libs.private: " + " ".join(required))

pc.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY_KEYSTONE_PC
    patched=1
  done

  if [[ "$patched" == "1" ]]; then
    log "Windows static pkg-config dependencies: shell32 ole32 uuid"
  fi
}

# Keystone 0.9.2's pkg-config template only advertises -lkeystone. Its bundled
# old LLVM Windows Path support also references SHGetKnownFolderPath,
# CoTaskMemFree, and KNOWNFOLDERID GUID constants. MSVC gets part of this via
# pragma comments, but MinGW static consumers need the dependencies expressed
# in keystone.pc so pkg-config/Meson can propagate them.
patch_windows_keystone_pkgconfig

probe_keystone() {
  local verbose="${1:-0}"
  local cflags libs reported_version link_log
  reported_version="$(PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --modversion keystone 2>/dev/null || true)"
  if ! PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --atleast-version="$KEYSTONE_PKGCONFIG_MIN_VERSION" keystone >/dev/null 2>&1; then
    if [[ "$verbose" == "1" ]]; then
      log "pkg-config reports Keystone version: ${reported_version:-unavailable} (required metadata >= ${KEYSTONE_PKGCONFIG_MIN_VERSION})"
    fi
    return 1
  fi
  cflags="$(PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --cflags keystone)"
  libs="$(PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --libs --static keystone)"
  link_log="${smoke_dir}/keystone-link.log"
  # Intentional token splitting: pkg-config returns compiler/linker flags.
  # shellcheck disable=SC2086
  if ! "$KEYSTONE_CXX" ${CXXFLAGS:-} $cflags "${smoke_dir}/keystone.cc" $libs ${LDFLAGS:-} -o "$smoke_output" >"$link_log" 2>&1; then
    if [[ "$verbose" == "1" ]]; then
      log "pkg-config reports Keystone version: ${reported_version:-unavailable}"
      log "target smoke-link failed; compiler output follows:"
      cat "$link_log" >&2 || true
    fi
    return 1
  fi
  if [[ ! -s "$smoke_output" ]]; then
    if [[ "$verbose" == "1" ]]; then
      log "target smoke-link returned success but expected output is missing or empty: ${smoke_output}"
      if [[ -s "$link_log" ]]; then
        log "compiler output follows:"
        cat "$link_log" >&2 || true
      fi
    fi
    return 1
  fi
  return 0
}

emit_environment() {
  if [[ -n "$env_file" ]]; then
    {
      printf 'XEMU_KEYSTONE_PKG_CONFIG_PATH=%q\n' "$pkg_path_joined"
      printf 'XEMU_KEYSTONE_TARGET_KIND=%q\n' "$target_kind"
      printf 'XEMU_KEYSTONE_TARGET_ARCH=%q\n' "$target_arch"
    } > "$env_file"
  fi
}

if probe_keystone; then
  version="$(PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --modversion keystone)"
  log "using existing Keystone ${version} for ${target_kind}/${target_arch}"
  emit_environment
  exit 0
fi

log "building Keystone ${KEYSTONE_VERSION} for ${target_kind}/${target_arch}"
log "C compiler: ${KEYSTONE_CC}"
log "C++ compiler: ${KEYSTONE_CXX}"
log "install prefix: ${KEYSTONE_PREFIX}"

cache_dir="${XEMU_KEYSTONE_CACHE_DIR:-${TMPDIR:-/tmp}/xemu-keystone-source-cache}"
archive="${cache_dir}/keystone-engine-${KEYSTONE_VERSION}.tar.gz"
source_dir="${TMPDIR:-/tmp}/xemu-keystone-source-${KEYSTONE_VERSION}"
build_dir="${TMPDIR:-/tmp}/keystone-build-${target_kind}-${target_arch}"
mkdir -p "$cache_dir"
verify_keystone_archive() {
  local candidate="$1" actual_sha
  if command -v sha256sum >/dev/null 2>&1; then
    actual_sha="$(sha256sum "$candidate" | awk '{print $1}')"
  else
    actual_sha="$(python3 - "$candidate" <<'PY_SHA256'
from pathlib import Path
import hashlib, sys
p = Path(sys.argv[1])
h = hashlib.sha256()
with p.open("rb") as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b""):
        h.update(chunk)
print(h.hexdigest())
PY_SHA256
)"
  fi
  if [[ "$actual_sha" != "$KEYSTONE_SOURCE_SHA256" ]]; then
    echo "Keystone source SHA-256 mismatch: expected ${KEYSTONE_SOURCE_SHA256}, got ${actual_sha}" >&2
    return 1
  fi
}

if [[ ! -s "$archive" ]]; then
  curl -fL --retry 3 --retry-delay 2 "$KEYSTONE_SOURCE_URL" -o "${archive}.tmp"
  if ! verify_keystone_archive "${archive}.tmp"; then
    rm -f "${archive}.tmp"
    exit 1
  fi
  mv "${archive}.tmp" "$archive"
elif ! verify_keystone_archive "$archive"; then
  log "discarding invalid cached Keystone source archive"
  rm -f "$archive"
  curl -fL --retry 3 --retry-delay 2 "$KEYSTONE_SOURCE_URL" -o "${archive}.tmp"
  if ! verify_keystone_archive "${archive}.tmp"; then
    rm -f "${archive}.tmp"
    exit 1
  fi
  mv "${archive}.tmp" "$archive"
fi
rm -rf "$source_dir" "$build_dir"
mkdir -p "$source_dir"
tar -xzf "$archive" -C "$source_dir" --strip-components=1

# PyPI's keystone-engine sdist wraps the native Keystone source below a
# packaging directory (normally src/), while the upstream tag archive places
# CMakeLists.txt at its top level. Resolve the real native source root instead
# of assuming one archive layout. Keep this deliberately narrow: the selected
# root must contain both Keystone's top-level CMakeLists.txt and bundled llvm/.
keystone_source_root=""
for candidate in "$source_dir" "$source_dir/src"; do
  if [[ -f "${candidate}/CMakeLists.txt" && -d "${candidate}/llvm" ]]; then
    keystone_source_root="$candidate"
    break
  fi
done
if [[ -z "$keystone_source_root" ]]; then
  while IFS= read -r cmake_file; do
    candidate="$(dirname "$cmake_file")"
    if [[ -d "${candidate}/llvm" ]]; then
      keystone_source_root="$candidate"
      break
    fi
  done < <(find "$source_dir" -maxdepth 3 -type f -name CMakeLists.txt -print 2>/dev/null)
fi
if [[ -z "$keystone_source_root" ]]; then
  echo "Keystone source archive extracted, but native CMake source root was not found" >&2
  echo "Archive: ${archive}" >&2
  echo "Extracted top-level entries:" >&2
  find "$source_dir" -mindepth 1 -maxdepth 2 -print 2>/dev/null | head -80 >&2 || true
  exit 1
fi
log "native source root: ${keystone_source_root}"

# Keystone 0.9.2 predates CMake 4 and GCC 15. Apply the same small source
# compatibility fixes locally instead of carrying a patched third-party tree.
# These do not change Keystone's assembler behavior.
python3 - "$keystone_source_root" <<'PY_KEYSTONE_COMPAT'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])

# Mirror the modern-CMake cleanup carried by current Keystone 0.9.2 distro
# recipes. Old CMP0051 behavior no longer exists in CMake 4.
def modernize_cmake(path: Path, *, top_level: bool = False) -> None:
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"cmake_minimum_required\(VERSION 2\.8(?:\.7)?\)",
                  "cmake_minimum_required(VERSION 3.10.0)", text, count=1)
    for policy in ("CMP0022", "CMP0051"):
        text = re.sub(
            rf"\n?if\s*\(POLICY {policy}\).*?endif\s*\(\)\s*\n?",
            "\n",
            text,
            flags=re.DOTALL,
        )
    if top_level:
        text = re.sub(
            r"\n?if\s*\(POLICY CMP0063\).*?endif\s*\(\)\s*\n?",
            "\n",
            text,
            flags=re.DOTALL,
        )
    path.write_text(text, encoding="utf-8")

modernize_cmake(root / "CMakeLists.txt", top_level=True)
modernize_cmake(root / "llvm/CMakeLists.txt")
modernize_cmake(root / "kstool/CMakeLists.txt")

# GCC 15+ no longer provides stdint types transitively in this old LLVM copy.
stl = root / "llvm/include/llvm/ADT/STLExtras.h"
if stl.is_file():
    text = stl.read_text(encoding="utf-8")
    marker = "#include <cstddef> // for std::size_t"
    if "#include <cstdint>" not in text:
        if marker in text:
            text = text.replace(marker, marker + "\n#include <cstdint>", 1)
        else:
            text = "#include <cstdint>\n" + text
    stl.write_text(text, encoding="utf-8")
PY_KEYSTONE_COMPAT

cmake_args=(
  -S "$keystone_source_root"
  -B "$build_dir"
  -G Ninja
  "-DCMAKE_C_COMPILER=${KEYSTONE_CC}"
  "-DCMAKE_CXX_COMPILER=${KEYSTONE_CXX}"
  "-DCMAKE_INSTALL_PREFIX=${KEYSTONE_PREFIX}"
  "-DPYTHON_EXECUTABLE=$(command -v python3)"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_STANDARD=14
  -DCMAKE_CXX_STANDARD_REQUIRED=ON
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_LIBS_ONLY=ON
  -DLLVM_TARGETS_TO_BUILD=X86
  -DLLVM_BUILD_TESTS=OFF
  -DLLVM_INCLUDE_TESTS=OFF
  -DLLVM_INCLUDE_EXAMPLES=OFF
  -DLLVM_INCLUDE_BENCHMARKS=OFF
  -DLLVM_ENABLE_TERMINFO=OFF
  -DLLVM_ENABLE_ZLIB=OFF
  -DLLVM_ENABLE_FFI=OFF
  -DLLVM_ENABLE_WERROR=OFF
)

if [[ -n "$KEYSTONE_CMAKE_LAUNCHER" ]]; then
  cmake_args+=(
    "-DCMAKE_C_COMPILER_LAUNCHER=${KEYSTONE_CMAKE_LAUNCHER}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${KEYSTONE_CMAKE_LAUNCHER}"
  )
fi
if [[ -n "$KEYSTONE_AR" ]]; then cmake_args+=("-DCMAKE_AR=${KEYSTONE_AR}"); fi
if [[ -n "$KEYSTONE_RANLIB" ]]; then cmake_args+=("-DCMAKE_RANLIB=${KEYSTONE_RANLIB}"); fi

case "$target_kind" in
  windows-cross)
    cmake_args+=(
      -DCMAKE_SYSTEM_NAME=Windows
      "-DCMAKE_SYSTEM_PROCESSOR=${target_arch}"
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    )
    ;;
  linux-cross)
    cmake_args+=(
      -DCMAKE_SYSTEM_NAME=Linux
      "-DCMAKE_SYSTEM_PROCESSOR=${target_arch}"
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    )
    ;;
  macos)
    cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=${target_arch}")
    if [[ -n "$macos_sdk" ]]; then cmake_args+=("-DCMAKE_OSX_SYSROOT=${macos_sdk}"); fi
    if [[ -n "$macos_min_version" ]]; then cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${macos_min_version}"); fi
    ;;
esac

if [[ -n "${CFLAGS:-}" ]]; then cmake_args+=("-DCMAKE_C_FLAGS=${CFLAGS}"); fi
if [[ -n "${CXXFLAGS:-}" || -n "${CFLAGS:-}" ]]; then
  cmake_args+=("-DCMAKE_CXX_FLAGS=${CFLAGS:-} ${CXXFLAGS:-}")
fi
if [[ -n "${LDFLAGS:-}" ]]; then
  cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=${LDFLAGS}" "-DCMAKE_SHARED_LINKER_FLAGS=${LDFLAGS}")
fi

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel --target keystone
cmake --install "$build_dir"

patch_windows_keystone_pkgconfig

if ! probe_keystone 1; then
  echo "Keystone built, but target pkg-config/link verification failed" >&2
  exit 1
fi
version="$(PKG_CONFIG_PATH="$probe_pkg_path" "$KEYSTONE_PKG_CONFIG" --modversion keystone)"
log "verified Keystone ${version} for ${target_kind}/${target_arch}"
emit_environment
