#!/usr/bin/env bash
# Cross-builds the Windows x64 driver (trinoodbc.dll) on macOS or Linux with mingw-w64, verifies it
# without Windows and stages it for the installer.
#
#   packaging/windows/cross_build_mingw.sh [options]
#
# Steps (each one is skipped when its output already exists, so the script can be re-run):
#   1. download zstd, lz4, simdjson and curl release tarballs into a cache and check them,
#   2. build them as STATIC Windows x64 libraries into a prefix (curl with Schannel + SSPI),
#   3. configure and build the driver with cmake/toolchain-mingw-w64.cmake (Ninja, Release,
#      static libgcc/libstdc++/winpthread),
#   4. verify the DLL with objdump: PE32+ x86-64, only Windows system DLL imports, the export table
#      equals src/platform/win/trinoodbc.def, resources (dialog, Trino logo icon, VERSIONINFO) present,
#   5. stage dist/windows/stage/{x64/trinoodbc.dll, docs/*.md, VERSION, powerbi/TrinoODBC.mez,
#      powerbi/README.md, assets/*} (assets = installer icon and bitmaps from
#      packaging/assets/generated, so the stage tree alone is enough for build_installer.sh).
#
# Requirements: x86_64-w64-mingw32-{gcc,g++,windres,objdump,strip}, cmake >= 3.16, ninja, curl,
# tar, and zip or python3 (for the Power BI .mez). macOS: brew install mingw-w64 cmake ninja.
# Debian/Ubuntu: apt install mingw-w64 cmake ninja-build curl (use the *-posix compiler variants).
#
# Runs under bash 3.2+ (macOS /bin/bash). Invoked from zsh or sh it re-executes itself with bash.

if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$script_dir/../.." && pwd)"

# ---- versions (override through the environment) ---------------------------------------------
ZSTD_VERSION="${ZSTD_VERSION:-1.5.7}"
LZ4_VERSION="${LZ4_VERSION:-1.10.0}"
SIMDJSON_VERSION="${SIMDJSON_VERSION:-3.13.0}"
CURL_VERSION="${CURL_VERSION:-8.14.1}"

# ---- defaults ---------------------------------------------------------------------------------
triple="x86_64-w64-mingw32"
build_dir="$repo/build-mingw"
prefix=""
work_dir=""
stage_dir="$repo/dist/windows/stage"
build_type="Release"
jobs=""
do_stage=1
verify_only=0
rebuild_deps=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

  --prefix DIR      install prefix for the static Windows dependencies
                    (default: <build-dir>/prefix)
  --build-dir DIR   CMake build directory of the driver (default: $repo/build-mingw)
  --work-dir DIR    downloads, sources and dependency build trees (default: <build-dir>/deps)
  --stage-dir DIR   staging directory for the installer (default: $repo/dist/windows/stage)
  --triple NAME     mingw-w64 tool prefix (default: x86_64-w64-mingw32)
  --build-type T    CMake build type of the driver (default: Release)
  --jobs N          parallel build jobs (default: number of CPUs)
  --rebuild-deps    rebuild the dependencies even if the prefix already has them
  --no-stage        build and verify only, do not touch the staging directory
  --verify-only     only run the DLL checks on an existing <build-dir>/trinoodbc.dll
  -h, --help        this text

Environment: ZSTD_VERSION ($ZSTD_VERSION), LZ4_VERSION ($LZ4_VERSION),
SIMDJSON_VERSION ($SIMDJSON_VERSION), CURL_VERSION ($CURL_VERSION).
EOF
}

die() { echo "error: $*" >&2; exit 1; }
log() { echo "==> $*"; }
# Prints a command (one argument per word, quoted when needed) and runs it.
run() {
  local out="+" a
  for a in "$@"; do
    case "$a" in
      *[[:space:]]*|"") out="$out '$a'" ;;
      *) out="$out $a" ;;
    esac
  done
  echo "$out"
  "$@"
}
need_arg() { [ $# -ge 2 ] && [ -n "$2" ] || die "option $1 needs a value"; }
abspath() {
  # Absolute path of a (possibly not yet existing) directory, without resolving symlinks.
  case "$1" in
    /*) echo "$1" ;;
    *) echo "$(pwd)/$1" ;;
  esac
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) need_arg "$@"; prefix="$(abspath "$2")"; shift 2 ;;
    --build-dir) need_arg "$@"; build_dir="$(abspath "$2")"; shift 2 ;;
    --work-dir) need_arg "$@"; work_dir="$(abspath "$2")"; shift 2 ;;
    --stage-dir) need_arg "$@"; stage_dir="$(abspath "$2")"; shift 2 ;;
    --triple) need_arg "$@"; triple="$2"; shift 2 ;;
    --build-type) need_arg "$@"; build_type="$2"; shift 2 ;;
    --jobs) need_arg "$@"; jobs="$2"; shift 2 ;;
    --rebuild-deps) rebuild_deps=1; shift ;;
    --no-stage) do_stage=0; shift ;;
    --verify-only) verify_only=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

[ -n "$prefix" ] || prefix="$build_dir/prefix"
[ -n "$work_dir" ] || work_dir="$build_dir/deps"
if [ -z "$jobs" ]; then
  jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
fi
case "$jobs" in ''|*[!0-9]*) die "--jobs must be a number (got '$jobs')" ;; esac

toolchain="$repo/cmake/toolchain-mingw-w64.cmake"
dll="$build_dir/trinoodbc.dll"
def_file="$repo/src/platform/win/trinoodbc.def"

# ---- tools ------------------------------------------------------------------------------------
find_tool() {
  # $1 = tool name; prints its path, trying the "-posix" variant (Debian/Ubuntu) for compilers.
  local t
  for t in "$1" "$1-posix"; do
    if command -v "$t" >/dev/null 2>&1; then command -v "$t"; return 0; fi
  done
  return 1
}
objdump_bin="$(find_tool "$triple-objdump")" || die "$triple-objdump not found (install mingw-w64)"
strip_bin="$(find_tool "$triple-strip")" || die "$triple-strip not found (install mingw-w64)"
if [ "$verify_only" -eq 0 ]; then
  cc_bin="$(find_tool "$triple-gcc")" || die "$triple-gcc not found (macOS: brew install mingw-w64)"
  cxx_bin="$(find_tool "$triple-g++")" || die "$triple-g++ not found (macOS: brew install mingw-w64)"
  rc_bin="$(find_tool "$triple-windres")" || die "$triple-windres not found (install mingw-w64)"
  command -v cmake >/dev/null 2>&1 || die "cmake not found"
  command -v ninja >/dev/null 2>&1 || die "ninja not found (brew install ninja / apt install ninja-build)"
  command -v curl >/dev/null 2>&1 || die "curl not found"
  command -v tar >/dev/null 2>&1 || die "tar not found"
  # The dependency builds and the driver use the mingw thread model "posix" (winpthread) for
  # std::thread / std::mutex; the win32 model of some Linux packages lacks them in GCC < 13.
  cxx_v="$("$cxx_bin" -v 2>&1)" || die "$cxx_bin -v failed"
  if ! grep -q 'Thread model: posix' <<< "$cxx_v"; then
    die "$cxx_bin does not use the posix thread model (Debian/Ubuntu: use the -posix variant, 'update-alternatives --config $triple-g++')"
  fi
fi

# Host compiler settings (Homebrew's LDFLAGS=-L/opt/homebrew/lib, CFLAGS=-arch arm64, ...) must not
# leak into a Windows cross build: CMake seeds its flags from these variables.
for v in CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS RC RCFLAGS CMAKE_PREFIX_PATH CMAKE_TOOLCHAIN_FILE \
         CMAKE_GENERATOR PKG_CONFIG_PATH PKG_CONFIG_LIBDIR SDKROOT MACOSX_DEPLOYMENT_TARGET; do
  if [ -n "${!v:-}" ]; then
    echo "note: ignoring host environment variable $v for the cross build"
    unset "$v"
  fi
done

echo "Trino ODBC mingw-w64 cross build"
echo "  repo:       $repo"
echo "  build dir:  $build_dir"
echo "  prefix:     $prefix"
echo "  work dir:   $work_dir"
echo "  stage dir:  $stage_dir"
echo "  triple:     $triple   jobs: $jobs   build type: $build_type"
if [ "$verify_only" -eq 0 ]; then
  echo "  compiler:   $cxx_bin ($("$cxx_bin" -dumpfullversion 2>/dev/null || "$cxx_bin" -dumpversion))"
fi

# Common CMake arguments of every dependency build (called when the tools are known).
# CMAKE_POLICY_VERSION_MINIMUM lets CMake 4.x configure projects that still declare
# cmake_minimum_required() < 3.5.
set_dep_cmake_args() {
  dep_args=(
    -G Ninja
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    "-DTRINO_MINGW_TRIPLE=$triple"
    "-DCMAKE_C_COMPILER=$cc_bin"
    "-DCMAKE_CXX_COMPILER=$cxx_bin"
    "-DCMAKE_RC_COMPILER=$rc_bin"
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_INSTALL_PREFIX=$prefix"
    "-DCMAKE_PREFIX_PATH=$prefix"
    -DCMAKE_INSTALL_LIBDIR=lib
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  )
}

# ---- download + unpack ------------------------------------------------------------------------
# Known-good SHA-256 of the default versions. Other versions are only checked for being a valid
# archive with the expected top-level directory.
expected_sha256() {
  case "$1" in
    zstd-1.5.7.tar.gz) echo "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3" ;;
    lz4-1.10.0.tar.gz) echo "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b" ;;
    simdjson-3.13.0.tar.gz) echo "07a1bb3587aac18fd6a10a83fe4ab09f1100ab39f0cb73baea1317826b9f9e0d" ;;
    curl-8.14.1.tar.gz) echo "6766ada7101d292b42b8b15681120acd68effa4a9660935853cf6d61f0d984d4" ;;
    *) echo "" ;;
  esac
}
sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}
# $1 = file, $2 = expected top-level directory. True when the file is a readable gzip tarball whose
# entries all live under that directory.
valid_archive() {
  local listing
  [ -s "$1" ] || return 1
  case "$2" in */*|..*|"") return 1 ;; esac
  gzip -t "$1" 2>/dev/null || return 1
  listing="$(tar -tzf "$1" 2>/dev/null)" || return 1
  [ -n "$listing" ] || return 1
  # Every entry must be below the expected directory (no absolute paths, no "../", no strays).
  # No "grep -q" at the end of a pipe: with pipefail its early exit could SIGPIPE the writer.
  [ -z "$(printf '%s\n' "$listing" | grep -v -e "^$2/" -e "^$2\$" -e '^pax_global_header$' || true)" ]
}
# $1 = URL, $2 = file name in the cache, $3 = expected top-level directory. Prints nothing on success.
fetch() {
  local url="$1" name="$2" top="$3" dest="$work_dir/downloads/$2" want got
  mkdir -p "$work_dir/downloads"
  want="$(expected_sha256 "$name")"
  if [ -f "$dest" ] && valid_archive "$dest" "$top"; then
    got="$(sha256_of "$dest")"
    if [ -z "$want" ] || [ "$got" = "$want" ]; then
      log "using cached $name (sha256 $got)"
      return 0
    fi
    echo "warning: cached $name has sha256 $got, expected $want; downloading again" >&2
  fi
  log "downloading $url"
  rm -f "$dest.part"
  run curl -fL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$dest.part" "$url" \
    || die "download failed: $url"
  valid_archive "$dest.part" "$top" || { rm -f "$dest.part"; die "$url is not a valid .tar.gz with top-level directory $top/"; }
  got="$(sha256_of "$dest.part")"
  if [ -n "$want" ] && [ "$got" != "$want" ]; then
    rm -f "$dest.part"
    die "sha256 mismatch for $name: got $got, expected $want"
  fi
  mv "$dest.part" "$dest"
  echo "    sha256 $got"
}
# $1 = cached file name, $2 = top-level directory. Unpacks into $work_dir/src/$2 (fresh each time a
# build runs, so a half-extracted tree from an interrupted run is never reused).
unpack() {
  local src_root="$work_dir/src"
  mkdir -p "$src_root"
  rm -rf "${src_root:?}/$2"
  run tar -xzf "$work_dir/downloads/$1" -C "$src_root"
  [ -d "$src_root/$2" ] || die "unpacking $1 did not produce $src_root/$2"
}
# $1 = name, $2 = source dir (containing CMakeLists.txt), remaining = extra CMake arguments.
cmake_dep() {
  local name="$1" src="$2" bdir="$work_dir/build/$1"
  shift 2
  rm -rf "$bdir"
  log "configuring $name"
  run cmake -S "$src" -B "$bdir" "${dep_args[@]}" "$@"
  log "building $name"
  run cmake --build "$bdir" --parallel "$jobs"
  log "installing $name into $prefix"
  run cmake --install "$bdir"
}
# A dependency counts as built when its stamp (written after a successful build, install and
# check of exactly this version) and its main outputs exist. A version change or a build that was
# interrupted leaves no stamp, so it is rebuilt.
stamp_file() { echo "$prefix/share/trino-odbc-mingw/$1.stamp"; }
have_outputs() {
  local stamp f
  stamp="$(stamp_file "$1")"
  shift
  [ "$rebuild_deps" -eq 0 ] || return 1
  [ -f "$stamp" ] || return 1
  for f in "$@"; do [ -e "$prefix/$f" ] || return 1; done
  return 0
}
write_stamp() {
  mkdir -p "$prefix/share/trino-odbc-mingw"
  echo "$1 built $(date -u +%Y-%m-%dT%H:%M:%SZ) with $("$cxx_bin" -dumpfullversion 2>/dev/null || "$cxx_bin" -dumpversion)" > "$(stamp_file "$1")"
}

build_zstd() {
  local top="zstd-$ZSTD_VERSION"
  if have_outputs "$top" lib/libzstd.a include/zstd.h lib/cmake/zstd/zstdConfig.cmake; then
    log "zstd $ZSTD_VERSION: already in $prefix, skipping"; return 0
  fi
  fetch "https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/$top.tar.gz" "$top.tar.gz" "$top"
  unpack "$top.tar.gz" "$top"
  cmake_dep zstd "$work_dir/src/$top/build/cmake" \
    -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF -DZSTD_LEGACY_SUPPORT=OFF
  write_stamp "$top"
}

build_lz4() {
  local top="lz4-$LZ4_VERSION"
  if have_outputs "$top" lib/liblz4.a include/lz4.h lib/cmake/lz4/lz4Config.cmake; then
    log "lz4 $LZ4_VERSION: already in $prefix, skipping"; return 0
  fi
  fetch "https://github.com/lz4/lz4/releases/download/v$LZ4_VERSION/$top.tar.gz" "$top.tar.gz" "$top"
  unpack "$top.tar.gz" "$top"
  cmake_dep lz4 "$work_dir/src/$top/build/cmake" \
    -DBUILD_STATIC_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
  write_stamp "$top"
}

build_simdjson() {
  local top="simdjson-$SIMDJSON_VERSION"
  if have_outputs "$top" lib/libsimdjson.a include/simdjson.h lib/cmake/simdjson/simdjson-config.cmake; then
    log "simdjson $SIMDJSON_VERSION: already in $prefix, skipping"; return 0
  fi
  # GitHub's tag archive (the project publishes no release tarball); its top directory is
  # simdjson-<version>.
  fetch "https://github.com/simdjson/simdjson/archive/refs/tags/v$SIMDJSON_VERSION.tar.gz" "$top.tar.gz" "$top"
  unpack "$top.tar.gz" "$top"
  cmake_dep simdjson "$work_dir/src/$top" \
    -DSIMDJSON_JUST_LIBRARY=ON -DSIMDJSON_DEVELOPER_MODE=OFF \
    -DSIMDJSON_ENABLE_THREADS=ON
  write_stamp "$top"
}

build_curl() {
  local top="curl-$CURL_VERSION"
  if have_outputs "$top" lib/libcurl.a include/curl/curl.h lib/cmake/CURL/CURLConfig.cmake; then
    log "curl $CURL_VERSION: already in $prefix, skipping"; return 0
  fi
  fetch "https://curl.se/download/$top.tar.gz" "$top.tar.gz" "$top"
  unpack "$top.tar.gz" "$top"
  # Schannel for TLS (Windows certificate store, no CA bundle needed), SSPI for SPNEGO/Kerberos and
  # NTLM, Windows IDN (normaliz.dll). HTTP only, no optional third-party libraries at all.
  # CURL_CA_BUNDLE / CURL_CA_PATH are deliberately NOT passed: for a Windows target curl skips the
  # host CA probing on its own, whereas an explicit "-DCURL_CA_BUNDLE=none" ends up compiled in as
  # the literal default CA file "none" (curl 8.14 with the policies of its minimum CMake version),
  # which would make Schannel ignore the Windows certificate store.
  cmake_dep curl "$work_dir/src/$top" \
    -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF -DBUILD_EXAMPLES=OFF -DCURL_USE_PKGCONFIG=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=OFF -DCURL_USE_WOLFSSL=OFF \
    -DCURL_USE_GNUTLS=OFF -DCURL_USE_RUSTLS=OFF -DCURL_WINDOWS_SSPI=ON -DCURL_USE_GSSAPI=OFF \
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=ON -DCURL_USE_LIBSSH2=OFF \
    -DCURL_USE_LIBSSH=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF \
    -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF -DUSE_MSH3=OFF -DUSE_LIBRTMP=OFF -DENABLE_ARES=OFF \
    -DENABLE_THREADED_RESOLVER=ON -DENABLE_IPV6=ON -DENABLE_UNICODE=ON -DHTTP_ONLY=ON \
    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_CA_FALLBACK=OFF
  check_curl_config "$work_dir/build/curl/lib/curl_config.h"
  write_stamp "$top"
}

# Fails when the configured libcurl lacks a feature the driver relies on on Windows, or carries a
# compiled-in default CA file/path (which would replace the Windows certificate store).
check_curl_config() {
  local h="$1" m cfg
  [ -f "$h" ] || die "$h not found after the curl build"
  cfg="$(cat "$h")"
  for m in USE_SCHANNEL USE_WINDOWS_SSPI USE_IPV6; do
    grep -q "^#define $m 1" <<< "$cfg" || die "libcurl was configured without $m ($h)"
  done
  for m in CURL_DISABLE_NEGOTIATE_AUTH CURL_DISABLE_KERBEROS_AUTH CURL_DISABLE_HTTP; do
    if grep -q "^#define $m" <<< "$cfg"; then die "libcurl was configured with $m ($h)"; fi
  done
  for m in CURL_CA_BUNDLE CURL_CA_PATH; do
    if grep -q "^#define $m" <<< "$cfg"; then
      die "libcurl has a compiled-in $m ($(grep "^#define $m" <<< "$cfg")); Schannel must use the Windows store"
    fi
  done
  echo "    libcurl: Schannel + SSPI (SPNEGO/Kerberos, NTLM), no compiled-in CA file"
}

# ---- driver -----------------------------------------------------------------------------------
build_driver() {
  [ -f "$prefix/lib/libcurl.a" ] || die "$prefix has no libcurl.a; the dependency step did not run"
  if [ -f "$build_dir/CMakeCache.txt" ]; then
    # Reusing a build directory configured for another platform/generator fails in obscure ways.
    cached_tc="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Z]*=//p' "$build_dir/CMakeCache.txt" | head -1)"
    if [ "$cached_tc" != "$toolchain" ]; then
      die "$build_dir was configured without $toolchain; remove it or pass another --build-dir"
    fi
    # find_package() results are cached: after switching to another prefix the old CURL_DIR,
    # zstd_DIR, ... would silently stay in use. Start the configuration afresh in that case.
    cached_prefix="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "$build_dir/CMakeCache.txt" | head -1)"
    if [ "$cached_prefix" != "$prefix" ]; then
      log "dependency prefix changed ($cached_prefix -> $prefix): discarding the CMake cache"
      run rm -rf "$build_dir/CMakeCache.txt" "$build_dir/CMakeFiles"
    fi
  fi
  log "configuring the driver"
  run cmake -S "$repo" -B "$build_dir" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" \
    "-DTRINO_MINGW_TRIPLE=$triple" \
    "-DCMAKE_C_COMPILER=$cc_bin" "-DCMAKE_CXX_COMPILER=$cxx_bin" "-DCMAKE_RC_COMPILER=$rc_bin" \
    "-DCMAKE_PREFIX_PATH=$prefix" \
    "-DCMAKE_BUILD_TYPE=$build_type" \
    -DTRINO_ODBC_DRIVER_MANAGER=windows \
    -DTRINO_ODBC_BUILD_TESTS=OFF \
    -DTRINO_ODBC_WITH_ZSTD=ON -DTRINO_ODBC_WITH_LZ4=ON \
    -DTRINO_ODBC_USE_SYSTEM_SIMDJSON=ON
  # Only the driver: the installer ships nothing else, and tools/trino_odbc_bench.cpp does not
  # compile for Windows yet (it includes <sql.h> before <windows.h>).
  log "building the driver (target trinoodbc)"
  run cmake --build "$build_dir" --parallel "$jobs" --target trinoodbc
  [ -f "$dll" ] || die "the build finished but $dll does not exist"
}

# ---- verification -----------------------------------------------------------------------------
# DLLs that are part of every supported Windows (10 1903+ / Server 2022+). Lower case.
system_dll_allowed() {
  case "$1" in
    kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|comctl32.dll|\
    comdlg32.dll|shlwapi.dll|version.dll|rpcrt4.dll|ntdll.dll|msvcrt.dll|ucrtbase.dll|\
    odbc32.dll|odbccp32.dll|ws2_32.dll|crypt32.dll|secur32.dll|bcrypt.dll|wldap32.dll|normaliz.dll|\
    iphlpapi.dll|userenv.dll|api-ms-win-crt-*.dll|api-ms-win-core-*.dll)
      return 0 ;;
  esac
  return 1
}

verify_dll() {
  local dump fmt sections imports bad d lower want_file got_file missing extra n_want n_got n_sql n_cfg
  [ -f "$dll" ] || die "$dll not found"
  log "verifying $dll"
  fmt="$("$objdump_bin" -f "$dll")" || die "objdump cannot read $dll"
  grep -q 'file format pei-x86-64' <<< "$fmt" || die "$dll is not a PE32+ x86-64 image:
$fmt"
  dump="$("$objdump_bin" -p "$dll")" || die "objdump -p failed on $dll"
  grep -q '^Magic[[:space:]]*020b' <<< "$dump" || die "$dll: optional header is not PE32+"
  # "DLL" appears as its own line under "Characteristics"; ASLR/DEP flags under DllCharacteristics.
  grep -q '^[[:space:]]*DLL[[:space:]]*$' <<< "$dump" || die "$dll: the DLL flag is not set in the PE header"
  grep -q '^[[:space:]]*DYNAMIC_BASE[[:space:]]*$' <<< "$dump" || die "$dll: not built with ASLR (DYNAMIC_BASE)"
  grep -q '^[[:space:]]*NX_COMPAT[[:space:]]*$' <<< "$dump" || die "$dll: not built with DEP (NX_COMPAT)"
  echo "  format: PE32+ x86-64 DLL, DYNAMIC_BASE, NX_COMPAT"

  # Imports: every "DLL Name:" line of the import directory.
  imports="$(echo "$dump" | awk '/DLL Name:/ {print $3}' | sort -fu)"
  [ -n "$imports" ] || die "no import table found in $dll"
  echo "  imports:"
  bad=""
  while IFS= read -r d; do
    [ -n "$d" ] || continue
    lower="$(echo "$d" | tr '[:upper:]' '[:lower:]')"
    if system_dll_allowed "$lower"; then
      echo "    $d"
    else
      echo "    $d   <-- NOT a Windows system DLL"
      bad="$bad $d"
    fi
  done <<EOF
$imports
EOF
  [ -z "$bad" ] || die "$dll imports non-system DLLs:$bad (the runtime or a dependency was linked dynamically)"

  # Exports: the name pointer table must equal the .def file exactly.
  want_file="$(mktemp "${TMPDIR:-/tmp}/trino_def.XXXXXX")"
  got_file="$(mktemp "${TMPDIR:-/tmp}/trino_exp.XXXXXX")"
  sed -e 's/;.*$//' "$def_file" | awk 'f && NF {print $1} /^[[:space:]]*EXPORTS/ {f=1}' | sort -u > "$want_file"
  echo "$dump" | awk '
    /\[Ordinal\/Name Pointer\] Table/ {t=1; next}
    t && /^[[:space:]]*$/ {t=0}
    t && /\[[[:space:]]*[0-9]+\]/ {print $NF}' | sort -u > "$got_file"
  n_want="$(wc -l < "$want_file" | tr -d ' ')"
  n_got="$(wc -l < "$got_file" | tr -d ' ')"
  missing="$(comm -23 "$want_file" "$got_file" | tr '\n' ' ')"
  extra="$(comm -13 "$want_file" "$got_file" | tr '\n' ' ')"
  n_sql="$(grep -c '^SQL' "$got_file" || true)"
  n_cfg="$(grep -c '^Config' "$got_file" || true)"
  rm -f "$want_file" "$got_file"
  [ "$n_want" -gt 0 ] || die "no EXPORTS found in $def_file"
  [ -z "$missing" ] || die "exports missing from $dll: $missing"
  [ -z "$extra" ] || die "$dll exports names that are not in trinoodbc.def: $extra"
  [ "$n_want" -eq "$n_got" ] || die "export count mismatch ($n_got vs $n_want)"
  echo "  exports: $n_got names, identical to $(basename "$def_file") ($n_sql SQL*, $n_cfg Config*)"

  # Resources: the DSN dialog and VERSIONINFO live in .rsrc (windres output linked in).
  sections="$("$objdump_bin" -h "$dll")" || die "objdump -h failed on $dll"
  grep -q '\.rsrc' <<< "$sections" || die "$dll has no .rsrc section (trinoodbc.rc not linked)"
  # Resource strings are UTF-16LE: look for the dialog caption, the time zone note and the
  # VERSIONINFO product version; the resource directory must hold the icon group IDI_TRINOODBC (1).
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$dll" "$(project_version)" <<'PY' || die "$dll: dialog, icon or VERSIONINFO resource missing"
import struct, sys
data = open(sys.argv[1], "rb").read()
needles = ["Trino ODBC Driver - Data Source Setup", "Europe/Istanbul", "ProductVersion", sys.argv[2]]
missing = [n for n in needles if n.encode("utf-16-le") not in data]
if missing:
    sys.stderr.write("missing UTF-16 resource strings: %s\n" % ", ".join(missing))
    sys.exit(1)
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = struct.unpack_from("<H", data, pe + 20)[0]
rsrc = None
for i in range(nsec):
    off = pe + 24 + opt + 40 * i
    name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", data, off)
    if name.rstrip(b"\0") == b".rsrc":
        rsrc = (va, rawptr)
if rsrc is None:
    sys.exit("no .rsrc section")
va, base = rsrc
def entries(off):
    named, ids = struct.unpack_from("<HH", data, base + off + 12)
    for i in range(named + ids):
        name, target = struct.unpack_from("<II", data, base + off + 16 + 8 * i)
        yield name, target
def subdir(off, want):
    for name, target in entries(off):
        if name == want and target & 0x80000000:
            return target & 0x7FFFFFFF
    return None
group_type = subdir(0, 14)            # RT_GROUP_ICON
icon_type = subdir(0, 3)              # RT_ICON
group = subdir(group_type, 1) if group_type is not None else None
if group is None or icon_type is None:
    sys.exit("icon group IDI_TRINOODBC (1) or RT_ICON images missing")
lang_entry = next(entries(group))[1]
data_rva, size = struct.unpack_from("<II", data, base + lang_entry)
grp = data[base + data_rva - va: base + data_rva - va + size]
count = struct.unpack_from("<H", grp, 4)[0]
sizes = sorted({(grp[6 + 14 * i] or 256) for i in range(count)})
print("  resources: .rsrc with the DSN dialog, VERSIONINFO %s and the Trino logo (icon group 1, %d images: %s px)"
      % (sys.argv[2], count, "/".join(str(s) for s in sizes)))
PY
  else
    echo "  resources: .rsrc present (python3 not found, contents not checked)"
  fi
  log "all checks passed"
}

# ---- staging ----------------------------------------------------------------------------------
project_version() {
  local v
  v="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Z]*=//p' "$build_dir/CMakeCache.txt" 2>/dev/null | head -1)"
  if [ -z "$v" ]; then
    v="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$repo/CMakeLists.txt" | head -1)"
  fi
  [ -n "$v" ] || die "cannot determine the project version"
  echo "$v"
}

stage() {
  local version f
  version="$(project_version)"
  log "staging version $version into $stage_dir"
  # Only the paths this script produces are replaced; anything else in the stage is left alone.
  rm -rf "${stage_dir:?}/x64" "${stage_dir:?}/docs" "${stage_dir:?}/powerbi" "${stage_dir:?}/assets" "${stage_dir:?}/VERSION"
  mkdir -p "$stage_dir/x64" "$stage_dir/docs" "$stage_dir/powerbi" "$stage_dir/assets"
  # Strip the COFF symbol table (debug info stays in the build directory's copy).
  run "$strip_bin" --strip-unneeded -o "$stage_dir/x64/trinoodbc.dll" "$dll"
  run cp "$repo/README.md" "$stage_dir/docs/README.md"
  # LICENSE at the stage root feeds the installer's license page; copies also go into docs/.
  for f in LICENSE NOTICE; do
    if [ -f "$repo/$f" ]; then
      run cp "$repo/$f" "$stage_dir/$f"
      run cp "$repo/$f" "$stage_dir/docs/$f"
    fi
  done
  if [ -f "$repo/THIRD-PARTY-NOTICES.md" ]; then
    run cp "$repo/THIRD-PARTY-NOTICES.md" "$stage_dir/docs/THIRD-PARTY-NOTICES.md"
  fi
  for f in "$repo"/docs/*.md; do
    [ -f "$f" ] || continue
    cp "$f" "$stage_dir/docs/"
  done
  echo "+ cp $repo/docs/*.md $stage_dir/docs/"
  printf '%s' "$version" > "$stage_dir/VERSION"
  echo "+ VERSION = $version"
  if bash "$repo/powerbi/build_mez.sh" "$stage_dir/powerbi"; then
    :
  else
    echo "warning: powerbi/build_mez.sh failed; zipping TrinoODBC.pq directly" >&2
    rm -f "$stage_dir/powerbi/TrinoODBC.mez"
    python3 - "$repo/powerbi/TrinoODBC.pq" "$stage_dir/powerbi/TrinoODBC.mez" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[1], "TrinoODBC.pq")
PY
  fi
  [ -s "$stage_dir/powerbi/TrinoODBC.mez" ] || die "TrinoODBC.mez was not produced"
  run cp "$repo/powerbi/README.md" "$stage_dir/powerbi/README.md"
  # Installer branding (Trino logo; regenerate with packaging/assets/make_icons.sh): the NSIS / Inno
  # Setup scripts take the icon and bitmaps from <stage>/assets when no other folder is given.
  local assets_src="$repo/packaging/assets/generated"
  for f in trinoodbc.ico installer-welcome.bmp installer-header.bmp installer-small.bmp; do
    [ -f "$assets_src/$f" ] || die "missing $assets_src/$f (run packaging/assets/make_icons.sh)"
    run cp "$assets_src/$f" "$stage_dir/assets/$f"
  done

  # The staged (stripped) DLL must pass the same checks.
  local saved="$dll"
  dll="$stage_dir/x64/trinoodbc.dll"
  verify_dll
  dll="$saved"
  log "staged files:"
  (cd "$stage_dir" && find . -type f | sed 's|^\./|    |' | sort)
}

# ---- main -------------------------------------------------------------------------------------
if [ "$verify_only" -eq 1 ]; then
  verify_dll
  exit 0
fi

mkdir -p "$prefix" "$work_dir"
set_dep_cmake_args
build_zstd
build_lz4
build_simdjson
build_curl
build_driver
verify_dll
if [ "$do_stage" -eq 1 ]; then
  stage
fi
log "done: $dll"
