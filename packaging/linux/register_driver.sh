#!/bin/sh
# Registers (or unregisters) the Trino ODBC driver with the driver manager on Linux.
#
#   register_driver.sh [options] [DRIVER_PATH]
#
# Options:
#   --unregister        remove the driver registration instead of adding it
#   --name NAME         driver name (default: "Trino ODBC Driver")
#   --odbcinst FILE     odbcinst.ini to edit when odbcinst(1) is not available
#                       (default: $ODBCINSTINI, else /etc/odbcinst.ini)
#   --template FILE     odbcinst.ini template (default: src/platform/unix/odbcinst.ini.in next to
#                       this script, or the installed share/trinoodbc/odbcinst.ini.template)
#   --no-odbcinst       never call odbcinst(1); edit the INI file directly
#   -h, --help
#
# DRIVER_PATH defaults to the first libtrinoodbc.so found in the usual library directories.
# The script is idempotent: running it twice leaves exactly one registration with the given path,
# and unregistering a driver that is not registered is a no-op. It is used by CMake's install step
# (TRINO_ODBC_INSTALL_ODBCINST=ON), by build_packages.sh (embedded into the deb/rpm maintainer
# scripts) and by hand.
#
# Strategy: unixODBC's odbcinst(1) is used when available (it updates the file the driver manager
# really reads, honours ODBCSYSINI and keeps UsageCount consistent). Otherwise the INI file is
# edited with awk. Either path works for unixODBC and for iODBC (same file format).
set -eu

TRINO_ODBC_DRIVER_NAME="${TRINO_ODBC_DRIVER_NAME:-Trino ODBC Driver}"
TRINO_ODBC_VERSION="${TRINO_ODBC_VERSION:-1.0.0}"
ACTION=register
DRIVER_PATH=""
ODBCINST_FILE="${ODBCINSTINI:-}"
TEMPLATE_FILE=""
USE_ODBCINST=1

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; }
log() { printf '%s\n' "register_driver: $*"; }
die() { printf '%s\n' "register_driver: error: $*" >&2; exit 1; }

# ---- argument parsing -----------------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --unregister) ACTION=unregister ;;
    --register) ACTION=register ;;
    --name) shift; [ $# -gt 0 ] || die "--name needs a value"; TRINO_ODBC_DRIVER_NAME="$1" ;;
    --name=*) TRINO_ODBC_DRIVER_NAME="${1#--name=}" ;;
    --odbcinst) shift; [ $# -gt 0 ] || die "--odbcinst needs a value"; ODBCINST_FILE="$1" ;;
    --odbcinst=*) ODBCINST_FILE="${1#--odbcinst=}" ;;
    --template) shift; [ $# -gt 0 ] || die "--template needs a value"; TEMPLATE_FILE="$1" ;;
    --template=*) TEMPLATE_FILE="${1#--template=}" ;;
    --no-odbcinst) USE_ODBCINST=0 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown option '$1' (see --help)" ;;
    *) [ -z "$DRIVER_PATH" ] || die "unexpected argument '$1'"; DRIVER_PATH="$1" ;;
  esac
  shift
done

script_dir() { cd "$(dirname "$0")" 2>/dev/null && pwd; }

# ---- locate the driver ----------------------------------------------------------------------------
find_driver() {
  for dir in /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 /opt/trinoodbc/lib \
             /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf \
             /usr/lib/i386-linux-gnu /usr/lib/trinoodbc /usr/local/lib/trinoodbc; do
    if [ -f "$dir/libtrinoodbc.so" ]; then printf '%s\n' "$dir/libtrinoodbc.so"; return 0; fi
  done
  return 1
}

if [ -z "$DRIVER_PATH" ]; then
  if [ "$ACTION" = register ]; then
    DRIVER_PATH="$(find_driver)" || die "libtrinoodbc.so not found; pass its path explicitly"
  else
    DRIVER_PATH="$(find_driver || true)"
  fi
fi
if [ "$ACTION" = register ]; then
  [ -f "$DRIVER_PATH" ] || die "driver library '$DRIVER_PATH' does not exist"
  # The driver manager needs an absolute path (it dlopen()s it from any working directory).
  case "$DRIVER_PATH" in
    /*) ;;
    *) DRIVER_PATH="$(cd "$(dirname "$DRIVER_PATH")" && pwd)/$(basename "$DRIVER_PATH")" ;;
  esac
fi

# ---- template rendering ---------------------------------------------------------------------------
find_template() {
  [ -n "$TEMPLATE_FILE" ] && { printf '%s\n' "$TEMPLATE_FILE"; return 0; }
  sd="$(script_dir)"
  for candidate in "$sd/../../src/platform/unix/odbcinst.ini.in" \
                   "$sd/odbcinst.ini.in" \
                   "$(dirname "$DRIVER_PATH")/../share/trinoodbc/odbcinst.ini.template" \
                   /usr/share/trinoodbc/odbcinst.ini.template \
                   /usr/local/share/trinoodbc/odbcinst.ini.template; do
    if [ -f "$candidate" ]; then printf '%s\n' "$candidate"; return 0; fi
  done
  return 1
}

# Escapes a value for use as the replacement side of a sed s|||-expression.
sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

# Writes the rendered odbcinst.ini fragment for this driver to stdout.
render_template() {
  tpl="$(find_template || true)"
  if [ -n "$tpl" ]; then
    sed -e "s|@TRINO_ODBC_DRIVER_NAME@|$(sed_escape "$TRINO_ODBC_DRIVER_NAME")|g" \
        -e "s|@TRINO_ODBC_DRIVER_PATH@|$(sed_escape "$DRIVER_PATH")|g" \
        -e "s|@TRINO_ODBC_VERSION@|$(sed_escape "$TRINO_ODBC_VERSION")|g" "$tpl"
    return 0
  fi
  # No template on disk (e.g. a hand-copied .so): the fragment is small enough to inline.
  cat <<EOF
[ODBC Drivers]
$TRINO_ODBC_DRIVER_NAME=Installed

[$TRINO_ODBC_DRIVER_NAME]
Description=Trino ODBC Driver $TRINO_ODBC_VERSION (ODBC 3.8, Unicode)
Driver=$DRIVER_PATH
APILevel=2
ConnectFunctions=YYN
DriverODBCVer=03.80
FileUsage=0
SQLLevel=1
UsageCount=1
EOF
}

# The driver section alone: drops the iODBC-style [ODBC Drivers] list and comment lines. odbcinst(1)
# installs every section of its input file as a driver, and the direct-INI path merges the list entry
# separately, so both paths feed on this.
render_driver_section() {
  render_template | awk '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); keep = (h != "ODBC Drivers") }
    keep && !/^[ \t]*[;#]/ { print }
  '
}

# ---- INI helpers (awk; POSIX) ---------------------------------------------------------------------
# Section names are compared literally (no regex), so any driver name is safe.
ini_remove_section() {  # FILE SECTION
  [ -f "$1" ] || return 0
  awk -v sec="$2" '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); skip = (h == sec) }
    !skip { print }
  ' "$1" > "$1.tmp.$$" && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
}

ini_remove_key() {  # FILE SECTION KEY
  [ -f "$1" ] || return 0
  awk -v sec="$2" -v key="$3" '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); insec = (h == sec); print; next }
    insec { k = $0; sub(/[ \t]*=.*$/, "", k); sub(/^[ \t]*/, "", k); if (k == key) next }
    { print }
  ' "$1" > "$1.tmp.$$" && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
}

ini_has_section() {  # FILE SECTION
  [ -f "$1" ] || return 1
  awk -v sec="$2" '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); if (h == sec) { found = 1; exit } }
    END { exit found ? 0 : 1 }
  ' "$1"
}

# Drops trailing blank lines so appended sections are separated by exactly one blank line.
ini_trim_trailing_blank() {  # FILE
  [ -f "$1" ] || return 0
  awk '/^[ \t]*$/ { blank = blank $0 "\n"; next } { printf "%s", blank; blank = ""; print }' "$1" > "$1.tmp.$$" \
    && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
}

# Appends stdin to FILE as a new block (one blank line separates it from existing content).
ini_append_block() {  # FILE
  ini_trim_trailing_blank "$1"
  { if [ -s "$1" ]; then printf '\n'; fi; cat; } >> "$1"
}

ini_add_key() {  # FILE SECTION KEY VALUE  (inserts right after the section header, or appends a new section)
  if ini_has_section "$1" "$2"; then
    awk -v sec="$2" -v line="$3=$4" '
      { print }
      /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); if (h == sec && !done) { print line; done = 1 } }
    ' "$1" > "$1.tmp.$$" && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
  else
    printf '[%s]\n%s=%s\n' "$2" "$3" "$4" | ini_append_block "$1"
  fi
}

ini_registered_path() {  # FILE SECTION -> prints Driver= value of the section
  [ -f "$1" ] || return 0
  awk -v sec="$2" '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); insec = (h == sec); next }
    insec { k = $0; sub(/[ \t]*=.*$/, "", k); sub(/^[ \t]*/, "", k);
            if (tolower(k) == "driver") { v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*$/, "", v); print v; exit } }
  ' "$1"
}

# ---- odbcinst(1) path -----------------------------------------------------------------------------
odbcinst_available() {
  [ "$USE_ODBCINST" = 1 ] && [ -z "$ODBCINST_FILE" ] && command -v odbcinst >/dev/null 2>&1
}

odbcinst_registered_path() {
  odbcinst -q -d -n "$TRINO_ODBC_DRIVER_NAME" 2>/dev/null | awk -F= 'tolower($1) ~ /^[ \t]*driver[ \t]*$/ { v = $2; sub(/^[ \t]*/, "", v); sub(/[ \t]*$/, "", v); print v; exit }'
}

odbcinst_unregister_all() {
  # odbcinst -u decrements UsageCount and only removes the section when it reaches zero, so loop
  # (bounded) until the driver is gone.
  n=0
  while [ -n "$(odbcinst_registered_path)" ] && [ "$n" -lt 20 ]; do
    odbcinst -u -d -n "$TRINO_ODBC_DRIVER_NAME" >/dev/null 2>&1 || break
    n=$((n + 1))
  done
  [ -z "$(odbcinst_registered_path)" ]
}

register_with_odbcinst() {
  current="$(odbcinst_registered_path)"
  if [ "$current" = "$DRIVER_PATH" ]; then
    log "'$TRINO_ODBC_DRIVER_NAME' is already registered with $DRIVER_PATH (odbcinst)"
    return 0
  fi
  if [ -n "$current" ]; then
    log "replacing existing registration ($current)"
    odbcinst_unregister_all || die "could not remove the existing registration of '$TRINO_ODBC_DRIVER_NAME'"
  fi
  tmp="$(mktemp "${TMPDIR:-/tmp}/trinoodbc-odbcinst.XXXXXX")"
  trap 'rm -f "$tmp"' EXIT
  render_driver_section > "$tmp"
  odbcinst -i -d -f "$tmp" >/dev/null || die "odbcinst -i -d failed (run as root, or set ODBCSYSINI to a writable directory)"
  rm -f "$tmp"
  trap - EXIT
  log "registered '$TRINO_ODBC_DRIVER_NAME' -> $DRIVER_PATH (odbcinst)"
}

unregister_with_odbcinst() {
  if [ -z "$(odbcinst_registered_path)" ]; then
    log "'$TRINO_ODBC_DRIVER_NAME' is not registered (odbcinst); nothing to do"
    return 0
  fi
  odbcinst_unregister_all || die "odbcinst -u -d failed (run as root?)"
  log "unregistered '$TRINO_ODBC_DRIVER_NAME' (odbcinst)"
}

# ---- direct INI editing path ----------------------------------------------------------------------
ini_file() {
  if [ -n "$ODBCINST_FILE" ]; then printf '%s\n' "$ODBCINST_FILE"
  elif [ -n "${ODBCSYSINI:-}" ]; then printf '%s\n' "$ODBCSYSINI/odbcinst.ini"
  else printf '%s\n' /etc/odbcinst.ini
  fi
}

register_with_ini() {
  file="$(ini_file)"
  dir="$(dirname "$file")"
  [ -d "$dir" ] || mkdir -p "$dir" || die "cannot create $dir"
  [ -f "$file" ] || : > "$file" || die "cannot create $file"
  [ -w "$file" ] || die "$file is not writable (run as root?)"
  current="$(ini_registered_path "$file" "$TRINO_ODBC_DRIVER_NAME")"
  if [ "$current" = "$DRIVER_PATH" ] && ini_has_section "$file" "ODBC Drivers" \
     && grep -q "^[[:space:]]*$(printf '%s' "$TRINO_ODBC_DRIVER_NAME" | sed 's/[][\\.*^$/]/\\&/g')[[:space:]]*=" "$file"; then
    log "'$TRINO_ODBC_DRIVER_NAME' is already registered with $DRIVER_PATH in $file"
    return 0
  fi
  ini_remove_section "$file" "$TRINO_ODBC_DRIVER_NAME"
  ini_remove_key "$file" "ODBC Drivers" "$TRINO_ODBC_DRIVER_NAME"
  ini_add_key "$file" "ODBC Drivers" "$TRINO_ODBC_DRIVER_NAME" "Installed"
  # Append the driver section only (the [ODBC Drivers] entry was merged above).
  tmp="$(mktemp "${TMPDIR:-/tmp}/trinoodbc-odbcinst.XXXXXX")"
  trap 'rm -f "$tmp"' EXIT
  render_driver_section > "$tmp"
  ini_append_block "$file" < "$tmp"
  rm -f "$tmp"
  trap - EXIT
  log "registered '$TRINO_ODBC_DRIVER_NAME' -> $DRIVER_PATH in $file"
}

unregister_with_ini() {
  file="$(ini_file)"
  if [ ! -f "$file" ] || ! ini_has_section "$file" "$TRINO_ODBC_DRIVER_NAME"; then
    log "'$TRINO_ODBC_DRIVER_NAME' is not registered in $file; nothing to do"
    return 0
  fi
  [ -w "$file" ] || die "$file is not writable (run as root?)"
  ini_remove_section "$file" "$TRINO_ODBC_DRIVER_NAME"
  ini_remove_key "$file" "ODBC Drivers" "$TRINO_ODBC_DRIVER_NAME"
  log "unregistered '$TRINO_ODBC_DRIVER_NAME' from $file"
}

# ---- main -----------------------------------------------------------------------------------------
main() {
  if [ "$ACTION" = register ]; then
    if odbcinst_available; then register_with_odbcinst; else register_with_ini; fi
  else
    if odbcinst_available; then unregister_with_odbcinst; else unregister_with_ini; fi
  fi
}

main
