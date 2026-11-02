#!/bin/bash
# Builds the Windows x64 installer of the Trino ODBC driver with NSIS, on macOS or Linux (no Windows
# machine needed).
#
#   build_installer.sh [options]
#
# Options:
#   --stage DIR       staging tree (default: dist/windows/stage), laid out as
#                       x64/trinoodbc.dll        the driver, plus the non-system DLLs it imports
#                       docs/*.md                documentation
#                       VERSION                  version (first line)
#                       powerbi/TrinoODBC.mez    Power BI custom connector
#                       powerbi/README.md        (optional)
#                       LICENSE                  (optional; enables the license page)
#                       assets/                  (optional) installer branding, see --assets
#   --assets DIR      folder with the Trino logo branding: trinoodbc.ico (installer, uninstaller and
#                     Add/Remove Programs icon), installer-welcome.bmp (164x314, welcome/finish
#                     pages), installer-header.bmp (150x57, page header). Default:
#                     packaging/assets/generated (packaging/assets/make_icons.sh), else <stage>/assets
#   --out DIR         output directory (default: dist/windows)
#   --version V       product version (default: <stage>/VERSION, else project() in CMakeLists.txt)
#   --makensis PATH   makensis to use (default: $MAKENSIS, then makensis on PATH, then Homebrew's)
#   --no-powerbi      build without the Power BI connector component (powerbi/ is not needed)
#   --no-dep-check    skip the check that every DLL imported by the staged DLLs is either a Windows
#                     system DLL or staged in x64/
#   --no-werror       do not treat makensis warnings as errors
#   -h, --help
#
# Output: <out>/TrinoODBC-<version>-windows-x64-setup.exe. An existing file of that name is deleted
# first and rebuilt, so re-running the script is safe. Afterwards the installer contents are listed
# with 7z (7z, 7zz or 7za on PATH) when available.
# Requires NSIS 3 (brew install makensis, apt install nsis). The installer script is
# packaging/windows/installer.nsi; packaging/windows/README-installer.md describes the installer.
set -eu
set -o pipefail

# $0 is captured here: zsh sets $0 to the function name inside functions.
SCRIPT_PATH="$0"
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd -P)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
NSI="$SCRIPT_DIR/installer.nsi"
STAGE_DIR="$ROOT/dist/windows/stage"
OUT_DIR="$ROOT/dist/windows"
VERSION=""
MAKENSIS_BIN="${MAKENSIS:-}"
WITH_POWERBI=1
DEP_CHECK=1
WERROR=1
WORK_DIR=""
ASSETS_DIR=""

usage() { sed -n '2,33p' "$SCRIPT_PATH" | sed 's/^# \{0,1\}//'; }
log() { printf '%s\n' "build_installer: $*"; }
warn() { printf '%s\n' "build_installer: warning: $*" >&2; }
die() { printf '%s\n' "build_installer: error: $*" >&2; exit 1; }
cleanup() { if [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then rm -rf "$WORK_DIR"; fi; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

need_arg() { [ "$2" -ge 2 ] || die "option $1 needs a value (see --help)"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --stage) need_arg "$1" $#; shift; STAGE_DIR="$1" ;;
    --stage=*) STAGE_DIR="${1#*=}" ;;
    --assets) need_arg "$1" $#; shift; ASSETS_DIR="$1" ;;
    --assets=*) ASSETS_DIR="${1#*=}" ;;
    --out) need_arg "$1" $#; shift; OUT_DIR="$1" ;;
    --out=*) OUT_DIR="${1#*=}" ;;
    --version) need_arg "$1" $#; shift; VERSION="$1" ;;
    --version=*) VERSION="${1#*=}" ;;
    --makensis) need_arg "$1" $#; shift; MAKENSIS_BIN="$1" ;;
    --makensis=*) MAKENSIS_BIN="${1#*=}" ;;
    --no-powerbi) WITH_POWERBI=0 ;;
    --no-dep-check) DEP_CHECK=0 ;;
    --no-werror) WERROR=0 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1' (see --help)" ;;
  esac
  shift
done

# NSIS reads defines verbatim: a quote, '$' or backquote in a path would change the script's meaning.
check_nsis_safe_path() {
  case "$2" in
    *'"'*|*'$'*|*'`'*) die "$1 '$2' contains a character NSIS cannot take in a path (\", \$ or \`)" ;;
  esac
  case "$2" in
    *"
"*) die "$1 contains a newline" ;;
  esac
}

# Little-endian unsigned integers from a binary file (od is POSIX; no xxd/python needed). The bytes
# are collected across lines because BSD od ends its output with an empty line.
read_le() {
  od -An -tu1 -v -j "$2" -N "$3" "$1" |
    awk '{ for (i = 1; i <= NF; i++) b[n++] = $i }
         END { v = 0; for (i = n - 1; i >= 0; i--) v = v * 256 + b[i]; printf "%d\n", v }'
}
read_u16() { read_le "$1" "$2" 2; }
read_u32() { read_le "$1" "$2" 4; }
file_size() { wc -c < "$1" | tr -d ' '; }

# Fails unless $1 is a 64-bit (PE32+, x86-64) Windows DLL.
check_pe_x64_dll() {
  pe_file="$1"
  pe_size="$(file_size "$pe_file")"
  [ "$pe_size" -ge 512 ] || die "$pe_file is not a Windows DLL (only $pe_size bytes)"
  [ "$(read_u16 "$pe_file" 0)" -eq 23117 ] || die "$pe_file is not a Windows PE file (no MZ header)"
  pe_off="$(read_u32 "$pe_file" 60)"
  [ $((pe_off + 26)) -le "$pe_size" ] || die "$pe_file is not a Windows PE file (bad PE header offset)"
  [ "$(read_u32 "$pe_file" "$pe_off")" -eq 17744 ] || die "$pe_file is not a Windows PE file (no PE signature)"
  pe_machine="$(read_u16 "$pe_file" $((pe_off + 4)))"
  [ "$pe_machine" -eq 34404 ] || die "$pe_file is not an x64 binary (PE machine $(printf '0x%04x' "$pe_machine"); 0x8664 expected)"
  pe_chars="$(read_u16 "$pe_file" $((pe_off + 22)))"
  [ $((pe_chars & 8192)) -ne 0 ] || die "$pe_file is an executable, not a DLL"
  [ "$(read_u16 "$pe_file" $((pe_off + 24)))" -eq 523 ] || die "$pe_file has no PE32+ optional header"
}

# DLLs every supported Windows (10 1903+) ships in System32, as the lower-case import names.
is_system_dll() {
  case "$1" in
    api-ms-win-*|ext-ms-win-*) return 0 ;;
    kernel32.dll|kernelbase.dll|ntdll.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|shlwapi.dll) return 0 ;;
    ole32.dll|oleaut32.dll|combase.dll|comctl32.dll|comdlg32.dll|shcore.dll|uxtheme.dll|dwmapi.dll) return 0 ;;
    ws2_32.dll|wsock32.dll|mswsock.dll|iphlpapi.dll|dnsapi.dll|winhttp.dll|wininet.dll|urlmon.dll) return 0 ;;
    crypt32.dll|secur32.dll|sspicli.dll|bcrypt.dll|bcryptprimitives.dll|ncrypt.dll|cryptbase.dll) return 0 ;;
    wintrust.dll|credui.dll|cryptui.dll|wldap32.dll|normaliz.dll|netapi32.dll|mpr.dll|userenv.dll) return 0 ;;
    odbc32.dll|odbccp32.dll|msvcrt.dll|ucrtbase.dll|rpcrt4.dll|version.dll|psapi.dll|dbghelp.dll) return 0 ;;
    winmm.dll|setupapi.dll|cfgmgr32.dll|imm32.dll|msimg32.dll|powrprof.dll|winspool.drv) return 0 ;;
    icu.dll|icuuc.dll|icuin.dll) return 0 ;;
  esac
  return 1
}

is_vc_runtime_dll() {
  case "$1" in
    vcruntime140*.dll|msvcp140*.dll|concrt140.dll|vccorlib140.dll) return 0 ;;
  esac
  return 1
}

# grep reads the whole output (no -q): an early exit would SIGPIPE objdump and fail under pipefail.
find_objdump() {
  for od_candidate in x86_64-w64-mingw32-objdump llvm-objdump objdump; do
    if command -v "$od_candidate" >/dev/null 2>&1 &&
       "$od_candidate" -p "$STAGE_DIR/x64/trinoodbc.dll" 2>/dev/null | grep 'DLL Name:' >/dev/null; then
      command -v "$od_candidate"
      return 0
    fi
  done
  return 1
}

find_makensis() {
  if [ -n "$MAKENSIS_BIN" ]; then
    command -v "$MAKENSIS_BIN" 2>/dev/null || { [ -x "$MAKENSIS_BIN" ] && printf '%s\n' "$MAKENSIS_BIN"; } || return 1
    return 0
  fi
  if command -v makensis >/dev/null 2>&1; then command -v makensis; return 0; fi
  for mk_candidate in /opt/homebrew/bin/makensis /usr/local/bin/makensis /usr/bin/makensis; do
    if [ -x "$mk_candidate" ]; then printf '%s\n' "$mk_candidate"; return 0; fi
  done
  return 1
}

# makensis 3.x on macOS aborts with std::bad_alloc while generating the language tables when the
# locale is "C"/POSIX: give it a UTF-8 locale unless the caller already has one.
ensure_utf8_locale() {
  loc_charmap="$(locale charmap 2>/dev/null || true)"
  case "$loc_charmap" in
    UTF-8|utf-8|UTF8|utf8) return 0 ;;
  esac
  loc_pick="$(locale -a 2>/dev/null | grep -i -E '^(c\.utf-?8|en_us\.utf-?8|utf-?8)$' | head -n 1 || true)"
  if [ -n "$loc_pick" ]; then
    log "locale charmap is '${loc_charmap:-unknown}'; running makensis with LC_ALL=$loc_pick"
    LC_ALL="$loc_pick"
    export LC_ALL
  else
    warn "no UTF-8 locale found (locale -a); makensis may fail under the '${loc_charmap:-unknown}' charmap"
  fi
}

# ---- stage tree -----------------------------------------------------------------------------------------
[ -f "$NSI" ] || die "installer script not found: $NSI"
[ -d "$STAGE_DIR" ] || die "stage directory not found: $STAGE_DIR (build and stage the Windows driver first, or pass --stage DIR)"
STAGE_DIR="$(cd "$STAGE_DIR" && pwd -P)"
check_nsis_safe_path "stage directory" "$STAGE_DIR"
log "stage: $STAGE_DIR"

DRIVER_DLL="$STAGE_DIR/x64/trinoodbc.dll"
[ -f "$DRIVER_DLL" ] || die "missing $DRIVER_DLL"
check_pe_x64_dll "$DRIVER_DLL"
log "driver: x64/trinoodbc.dll ($(file_size "$DRIVER_DLL") bytes, PE32+ x86-64 DLL)"

DOC_COUNT="$(find "$STAGE_DIR/docs" -maxdepth 1 -type f -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
[ "$DOC_COUNT" -gt 0 ] || die "no documentation in $STAGE_DIR/docs (docs/*.md expected)"
log "docs: $DOC_COUNT file(s) in docs/"

if [ "$WITH_POWERBI" -eq 1 ]; then
  MEZ="$STAGE_DIR/powerbi/TrinoODBC.mez"
  [ -f "$MEZ" ] || die "missing $MEZ (build it with powerbi/build_mez.sh $STAGE_DIR/powerbi, or pass --no-powerbi)"
  # A .mez is a ZIP archive: local file header signature "PK\003\004".
  [ "$(read_u32 "$MEZ" 0)" -eq 67324752 ] || die "$MEZ is not a ZIP archive (a .mez must be one)"
  log "power bi: powerbi/TrinoODBC.mez ($(file_size "$MEZ") bytes)"
  if [ ! -f "$STAGE_DIR/powerbi/README.md" ]; then
    warn "no powerbi/README.md in the stage; the installer ships the connector without it"
  fi
else
  log "power bi: component disabled (--no-powerbi)"
fi

# Installer branding (Trino logo): icon + wizard bitmaps.
ASSET_FILES="trinoodbc.ico installer-welcome.bmp installer-header.bmp"
if [ -z "$ASSETS_DIR" ]; then
  for assets_candidate in "$ROOT/packaging/assets/generated" "$STAGE_DIR/assets"; do
    if [ -f "$assets_candidate/trinoodbc.ico" ]; then ASSETS_DIR="$assets_candidate"; break; fi
  done
  [ -n "$ASSETS_DIR" ] || die "no installer branding found in $ROOT/packaging/assets/generated or $STAGE_DIR/assets (run packaging/assets/make_icons.sh, or pass --assets DIR)"
fi
[ -d "$ASSETS_DIR" ] || die "assets directory not found: $ASSETS_DIR"
ASSETS_DIR="$(cd "$ASSETS_DIR" && pwd -P)"
check_nsis_safe_path "assets directory" "$ASSETS_DIR"
for asset in $ASSET_FILES; do
  [ -f "$ASSETS_DIR/$asset" ] || die "missing $ASSETS_DIR/$asset (run packaging/assets/make_icons.sh)"
done
# .ico header: reserved 0, type 1; .bmp: "BM" and the size NSIS expects.
[ "$(read_u16 "$ASSETS_DIR/trinoodbc.ico" 0)" -eq 0 ] && [ "$(read_u16 "$ASSETS_DIR/trinoodbc.ico" 2)" -eq 1 ] ||
  die "$ASSETS_DIR/trinoodbc.ico is not a Windows icon file"
check_bmp() {
  [ "$(read_u16 "$1" 0)" -eq 19778 ] || die "$1 is not a BMP file"
  bmp_w="$(read_u32 "$1" 18)"; bmp_h="$(read_u32 "$1" 22)"
  [ "$bmp_w" -eq "$2" ] && [ "$bmp_h" -eq "$3" ] || warn "$1 is ${bmp_w}x${bmp_h}; NSIS expects $2x$3 (it is stretched)"
}
check_bmp "$ASSETS_DIR/installer-welcome.bmp" 164 314
check_bmp "$ASSETS_DIR/installer-header.bmp" 150 57
log "branding: $ASSETS_DIR (trinoodbc.ico $(read_u16 "$ASSETS_DIR/trinoodbc.ico" 4) image(s), installer-welcome.bmp, installer-header.bmp)"

# Dependency DLLs staged next to the driver; they are installed next to it as well.
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/trinoodbc-nsis.XXXXXX")" || die "cannot create a temporary directory"
check_nsis_safe_path "temporary directory" "$WORK_DIR"
EXTRA_LIST="$WORK_DIR/extra-dlls.txt"
find "$STAGE_DIR/x64" -maxdepth 1 -type f -iname '*.dll' ! -name 'trinoodbc.dll' | LC_ALL=C sort > "$EXTRA_LIST"
while IFS= read -r extra_path; do
  extra_name="$(basename "$extra_path")"
  case "$extra_name" in
    *[!A-Za-z0-9._+-]*) die "unsupported character in staged DLL name '$extra_name'" ;;
  esac
  check_pe_x64_dll "$extra_path"
  log "dependency: x64/$extra_name ($(file_size "$extra_path") bytes)"
done < "$EXTRA_LIST"
find "$STAGE_DIR/x64" -maxdepth 1 -type f ! -iname '*.dll' | while IFS= read -r other_path; do
  warn "not packaged (only DLLs are): x64/$(basename "$other_path")"
done

if [ "$DEP_CHECK" -eq 1 ]; then
  if OBJDUMP="$(find_objdump)"; then
    log "checking DLL imports with $OBJDUMP"
    STAGED_NAMES="$WORK_DIR/staged-names.txt"
    { printf '%s\n' trinoodbc.dll; while IFS= read -r p; do basename "$p"; done < "$EXTRA_LIST"; } |
      tr 'A-Z' 'a-z' > "$STAGED_NAMES"
    MISSING="$WORK_DIR/missing.txt"
    : > "$MISSING"
    { printf '%s\n' "$DRIVER_DLL"; cat "$EXTRA_LIST"; } > "$WORK_DIR/all-dlls.txt"
    while IFS= read -r dll_path; do
      "$OBJDUMP" -p "$dll_path" > "$WORK_DIR/objdump.txt" 2>&1 ||
        die "$OBJDUMP could not read $dll_path: $(head -n 3 "$WORK_DIR/objdump.txt")"
      sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' "$WORK_DIR/objdump.txt" | tr -d '\r' | tr 'A-Z' 'a-z' |
        LC_ALL=C sort -u > "$WORK_DIR/imports.txt"
      while IFS= read -r imp; do
        [ -n "$imp" ] || continue
        if is_system_dll "$imp" || grep -Fxq "$imp" "$STAGED_NAMES"; then
          continue
        fi
        if is_vc_runtime_dll "$imp"; then
          warn "$(basename "$dll_path") imports $imp: target machines need the Microsoft Visual C++ Redistributable (x64)"
          continue
        fi
        printf '%s (imported by %s)\n' "$imp" "$(basename "$dll_path")" >> "$MISSING"
      done < "$WORK_DIR/imports.txt"
    done < "$WORK_DIR/all-dlls.txt"
    if [ -s "$MISSING" ]; then
      while IFS= read -r m; do printf '%s\n' "build_installer: error: not staged and not a Windows system DLL: $m" >&2; done < "$MISSING"
      die "copy the missing DLLs into $STAGE_DIR/x64 (or link them statically); --no-dep-check skips this check"
    fi
    log "imports: every non-system DLL is staged"
  else
    warn "no objdump that reads PE files (x86_64-w64-mingw32-objdump, llvm-objdump); DLL import check skipped"
  fi
else
  log "imports: check skipped (--no-dep-check)"
fi

# ---- version ----------------------------------------------------------------------------------------------
VERSION_SOURCE="--version"
if [ -z "$VERSION" ] && [ -f "$STAGE_DIR/VERSION" ]; then
  VERSION="$(tr -d '\r' < "$STAGE_DIR/VERSION" | sed -n '/[^[:space:]]/{s/^[[:space:]]*//;s/[[:space:]]*$//;p;q;}')"
  VERSION_SOURCE="$STAGE_DIR/VERSION"
  # Tolerate "Trino ODBC Driver 1.0.0" or "version=1.0.0": take the first version-looking token.
  if ! printf '%s\n' "$VERSION" | grep -Eq '^[0-9]+(\.[0-9]+){0,3}([-+][0-9A-Za-z][0-9A-Za-z.+-]*)?$'; then
    VERSION="$(printf '%s\n' "$VERSION" | grep -Eo '[0-9]+(\.[0-9]+){1,3}([-+][0-9A-Za-z][0-9A-Za-z.+-]*)?' | head -n 1 || true)"
  fi
fi
if [ -z "$VERSION" ]; then
  VERSION="$(sed -n 's/^project(trinoodbc VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
  VERSION_SOURCE="$ROOT/CMakeLists.txt"
fi
[ -n "$VERSION" ] || die "no version: pass --version, or put it into $STAGE_DIR/VERSION"
printf '%s\n' "$VERSION" | grep -Eq '^[0-9]+(\.[0-9]+){0,3}([-+][0-9A-Za-z][0-9A-Za-z.+-]*)?$' ||
  die "version '$VERSION' (from $VERSION_SOURCE) is not of the form 1.2.3 or 1.2.3-rc1"

# 4-part numeric version for the version resource: suffix dropped, missing parts are 0.
VERSION_CORE="${VERSION%%[-+]*}"
IFS=. read -r v1 v2 v3 v4 <<EOF
$VERSION_CORE
EOF
VERSION_NUMERIC=""
for part in "$v1" "${v2:-0}" "${v3:-0}" "${v4:-0}"; do
  part="$(printf '%s' "$part" | sed 's/^0*\([0-9]\)/\1/')"
  [ "${#part}" -le 5 ] && [ "$part" -le 65535 ] || die "version part '$part' of '$VERSION' is larger than 65535"
  VERSION_NUMERIC="${VERSION_NUMERIC:+$VERSION_NUMERIC.}$part"
done
log "version: $VERSION (from $VERSION_SOURCE; version resource $VERSION_NUMERIC)"

# ---- license ------------------------------------------------------------------------------------------------
LICENSE_FILE=""
for lic in "$STAGE_DIR/LICENSE" "$STAGE_DIR/LICENSE.txt" "$STAGE_DIR/LICENSE.md" \
           "$ROOT/LICENSE" "$ROOT/LICENSE.txt" "$ROOT/LICENSE.md"; do
  if [ -f "$lic" ]; then LICENSE_FILE="$lic"; break; fi
done
if [ -n "$LICENSE_FILE" ]; then
  check_nsis_safe_path "license file" "$LICENSE_FILE"
  log "license page: $LICENSE_FILE"
else
  log "license page: skipped (no LICENSE in the stage or the repository)"
fi

# ---- output -------------------------------------------------------------------------------------------------
mkdir -p "$OUT_DIR" || die "cannot create output directory $OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd -P)"
check_nsis_safe_path "output directory" "$OUT_DIR"
EXE="$OUT_DIR/TrinoODBC-$VERSION-windows-x64-setup.exe"
if [ -e "$EXE" ]; then
  log "removing previous $EXE"
  rm -f "$EXE"
fi

# Install/uninstall macros for the dependency DLLs (empty when the driver has none).
EXTRA_NSH="$WORK_DIR/extra_dlls.nsh"
{
  printf '%s\n' "; Generated by build_installer.sh: dependency DLLs staged next to trinoodbc.dll."
  printf '%s\n' "!macro TRINO_INSTALL_EXTRA_DLLS"
  while IFS= read -r extra_path; do
    extra_name="$(basename "$extra_path")"
    printf '  !insertmacro TRINO_INSTALL_DLL "%s" "%s"\n' "$extra_path" "$extra_name"
  done < "$EXTRA_LIST"
  printf '%s\n' "!macroend" "!macro TRINO_UNINSTALL_EXTRA_DLLS"
  while IFS= read -r extra_path; do
    extra_name="$(basename "$extra_path")"
    printf '  Delete /REBOOTOK "$INSTDIR\\%s"\n' "$extra_name"
    printf '  Delete /REBOOTOK "$INSTDIR\\%s.old"\n' "$extra_name"
    printf '  Delete /REBOOTOK "$INSTDIR\\%s.new"\n' "$extra_name"
  done < "$EXTRA_LIST"
  printf '%s\n' "!macroend"
} > "$EXTRA_NSH"

# ---- makensis -----------------------------------------------------------------------------------------------
MAKENSIS_PATH="$(find_makensis)" || die "makensis not found (brew install makensis / apt install nsis, or pass --makensis PATH)"
MAKENSIS_VERSION="$("$MAKENSIS_PATH" -VERSION 2>/dev/null | tr -d '\r' | head -n 1)"
case "$MAKENSIS_VERSION" in
  v3.*|3.*) ;;
  *) die "$MAKENSIS_PATH reports version '$MAKENSIS_VERSION'; NSIS 3 is required (Unicode installers)" ;;
esac
log "makensis: $MAKENSIS_PATH ($MAKENSIS_VERSION)"
ensure_utf8_locale

set -- -V2 -INPUTCHARSET UTF8 \
  "-DVERSION=$VERSION" "-DVERSION_NUMERIC=$VERSION_NUMERIC" \
  "-DSTAGE=$STAGE_DIR" "-DOUT=$OUT_DIR" "-DEXTRA_DLLS_NSH=$EXTRA_NSH" "-DASSETS=$ASSETS_DIR"
if [ "$WERROR" -eq 1 ]; then set -- -WX "$@"; fi
if [ -n "$LICENSE_FILE" ]; then set -- "$@" "-DLICENSE_FILE=$LICENSE_FILE"; fi
if [ "$WITH_POWERBI" -eq 0 ]; then set -- "$@" -DNO_POWERBI; fi
log "running: $MAKENSIS_PATH $* $NSI"
"$MAKENSIS_PATH" "$@" "$NSI" || die "makensis failed (exit $?)"

[ -s "$EXE" ] || die "makensis succeeded but $EXE was not written"
[ "$(read_u16 "$EXE" 0)" -eq 23117 ] || die "$EXE is not a Windows executable"

if command -v shasum >/dev/null 2>&1; then
  EXE_SHA256="$(shasum -a 256 "$EXE" | awk '{ print $1 }')"
elif command -v sha256sum >/dev/null 2>&1; then
  EXE_SHA256="$(sha256sum "$EXE" | awk '{ print $1 }')"
else
  EXE_SHA256="(no shasum/sha256sum)"
fi

# ---- contents -----------------------------------------------------------------------------------------------
SEVENZIP=""
for sz in 7z 7zz 7za; do
  if command -v "$sz" >/dev/null 2>&1; then SEVENZIP="$(command -v "$sz")"; break; fi
done
if [ -n "$SEVENZIP" ]; then
  log "contents ($SEVENZIP l):"
  "$SEVENZIP" l "$EXE" > "$WORK_DIR/listing.txt" 2>&1 || {
    cat "$WORK_DIR/listing.txt" >&2
    die "$SEVENZIP cannot list $EXE"
  }
  cat "$WORK_DIR/listing.txt"
  grep -q 'trinoodbc\.dll' "$WORK_DIR/listing.txt" || die "trinoodbc.dll is missing from the installer listing"
  grep -q 'trinoodbc\.ico' "$WORK_DIR/listing.txt" || die "trinoodbc.ico (Add/Remove Programs icon) is missing from the installer listing"
  if [ "$WITH_POWERBI" -eq 1 ]; then
    grep -q 'TrinoODBC\.mez' "$WORK_DIR/listing.txt" || die "TrinoODBC.mez is missing from the installer listing"
  fi
else
  warn "7z not found (brew install sevenzip / apt install p7zip-full); contents not listed"
fi

log "installer: $EXE"
log "size: $(file_size "$EXE") bytes, sha256: $EXE_SHA256"
log "on Windows (as administrator): run it, or silently: TrinoODBC-$VERSION-windows-x64-setup.exe /S [/NOPOWERBI] [/NODOCS] [/D=C:\\Path]"
