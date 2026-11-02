#!/bin/sh
# macOS installer postinstall: registers the Trino ODBC driver with the ONE driver manager the
# packaged library was built for:
#   iodbc     -> /Library/ODBC/odbcinst.ini (the system driver manager's file)
#   unixodbc  -> Homebrew unixODBC through odbcinst(1)
# The two are not interchangeable: SQLWCHAR is 4 bytes with iODBC and 2 bytes with unixODBC, and each
# build links that driver manager's installer library for DSN lookups. Registering a build with the
# other driver manager garbles every wide string and overruns the application's buffers.
#
# build_pkg.sh substitutes the two @...@ placeholders below when it stages this script; the build also
# records the driver manager in share/DRIVER_MANAGER, which is used when the script is run by hand.
#
# Installer.app runs this as root with $1 = package path, $2 = install destination ("/"), $3 = the
# target volume. It must be self-contained (no other packaging script is installed) and idempotent:
# re-installing or upgrading leaves exactly one registration.
set -eu

DRIVER_NAME="${TRINO_ODBC_DRIVER_NAME:-Trino ODBC Driver}"
PACKAGED_DRIVER_MANAGER="@TRINO_ODBC_DRIVER_MANAGER@"
PACKAGED_INSTALL_DIR="@TRINO_ODBC_INSTALL_DIR@"
INSTALL_ROOT="${2:-/}"
case "$INSTALL_ROOT" in */) INSTALL_ROOT="${INSTALL_ROOT%/}" ;; esac
case "$PACKAGED_INSTALL_DIR" in
  /*) ;;
  *) PACKAGED_INSTALL_DIR=/Library/ODBC/TrinoODBC ;;  # placeholder not substituted (run by hand)
esac
DRIVER_DIR="$INSTALL_ROOT$PACKAGED_INSTALL_DIR"
DRIVER_PATH="$DRIVER_DIR/lib/libtrinoodbc.dylib"
TEMPLATE="$DRIVER_DIR/share/odbcinst.ini.template"
ODBC_DIR="$INSTALL_ROOT/Library/ODBC"
ODBCINST_INI="$ODBC_DIR/odbcinst.ini"
VERSION="$(cat "$DRIVER_DIR/share/VERSION" 2>/dev/null || echo 1.0.0)"

log() { printf '%s\n' "trinoodbc postinstall: $*"; }

if [ ! -f "$DRIVER_PATH" ]; then
  log "error: $DRIVER_PATH was not installed" >&2
  exit 1
fi

DRIVER_MANAGER="$PACKAGED_DRIVER_MANAGER"
case "$DRIVER_MANAGER" in
  iodbc|unixodbc) ;;
  *) DRIVER_MANAGER="$(tr -d ' \t\r\n' < "$DRIVER_DIR/share/DRIVER_MANAGER" 2>/dev/null || true)" ;;
esac
case "$DRIVER_MANAGER" in
  iodbc|unixodbc) ;;
  *)
    log "error: cannot tell which driver manager $DRIVER_PATH was built for (no share/DRIVER_MANAGER); not registering" >&2
    exit 1
    ;;
esac

# ---- INI helpers (identical to packaging/linux/register_driver.sh; section names compared literally)
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

ini_trim_trailing_blank() {  # FILE
  [ -f "$1" ] || return 0
  awk '/^[ \t]*$/ { blank = blank $0 "\n"; next } { printf "%s", blank; blank = ""; print }' "$1" > "$1.tmp.$$" \
    && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
}

ini_append_block() {  # FILE  (stdin appended after exactly one blank line)
  ini_trim_trailing_blank "$1"
  { if [ -s "$1" ]; then printf '\n'; fi; cat; } >> "$1"
}

ini_add_key() {  # FILE SECTION KEY VALUE
  if ini_has_section "$1" "$2"; then
    awk -v sec="$2" -v line="$3=$4" '
      { print }
      /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); if (h == sec && !done) { print line; done = 1 } }
    ' "$1" > "$1.tmp.$$" && cat "$1.tmp.$$" > "$1" && rm -f "$1.tmp.$$"
  else
    printf '[%s]\n%s=%s\n' "$2" "$3" "$4" | ini_append_block "$1"
  fi
}

sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

# Rendered odbcinst.ini fragment for this installation (from the installed template, else inline).
render_fragment() {
  if [ -f "$TEMPLATE" ]; then
    sed -e "s|@TRINO_ODBC_DRIVER_NAME@|$(sed_escape "$DRIVER_NAME")|g" \
        -e "s|@TRINO_ODBC_DRIVER_PATH@|$(sed_escape "$DRIVER_PATH")|g" \
        -e "s|@TRINO_ODBC_VERSION@|$(sed_escape "$VERSION")|g" "$TEMPLATE"
    return 0
  fi
  cat <<EOF
[ODBC Drivers]
$DRIVER_NAME=Installed

[$DRIVER_NAME]
Description=Trino ODBC Driver $VERSION (ODBC 3.8, Unicode)
Driver=$DRIVER_PATH
APILevel=2
ConnectFunctions=YYN
DriverODBCVer=03.80
FileUsage=0
SQLLevel=1
UsageCount=1
EOF
}

# The driver section alone (no [ODBC Drivers] list, no comments): the iODBC path merges the list
# entry itself, and odbcinst(1) would install the list as a phantom driver called "ODBC Drivers".
render_driver_section() {
  render_fragment | awk '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); keep = (h != "ODBC Drivers") }
    keep && !/^[ \t]*[;#]/ { print }
  '
}

ini_registered_path() {  # FILE SECTION -> prints Driver= value of the section
  [ -f "$1" ] || return 0
  awk -v sec="$2" '
    /^[ \t]*\[/ { h = $0; sub(/^[ \t]*\[/, "", h); sub(/\][ \t]*$/, "", h); insec = (h == sec); next }
    insec { k = $0; sub(/[ \t]*=.*$/, "", k); sub(/^[ \t]*/, "", k);
            if (tolower(k) == "driver") { v = $0; sub(/^[^=]*=[ \t]*/, "", v); sub(/[ \t]*$/, "", v); print v; exit } }
  ' "$1"
}

# Homebrew unixODBC's odbcinst(1), if any (only consulted for an install onto the running system).
ODBCINST_BIN=""
if [ "$INSTALL_ROOT" = "" ]; then
  for candidate in /opt/homebrew/bin/odbcinst /usr/local/bin/odbcinst; do
    [ -x "$candidate" ] && { ODBCINST_BIN="$candidate"; break; }
  done
  [ -n "$ODBCINST_BIN" ] || ODBCINST_BIN="$(command -v odbcinst 2>/dev/null || true)"
fi

unixodbc_registered_path() {
  [ -n "$ODBCINST_BIN" ] || return 0
  "$ODBCINST_BIN" -q -d -n "$DRIVER_NAME" 2>/dev/null \
    | awk -F= 'tolower($1) ~ /^[ \t]*driver[ \t]*$/ { v = $2; sub(/^[ \t]*/, "", v); sub(/[ \t]*$/, "", v); print v; exit }'
}

unixodbc_unregister_all() {
  # odbcinst -u only decrements UsageCount until it reaches zero; loop (bounded) until it is gone.
  n=0
  while [ -n "$(unixodbc_registered_path)" ] && [ "$n" -lt 20 ]; do
    "$ODBCINST_BIN" -u -d -n "$DRIVER_NAME" >/dev/null 2>&1 || break
    n=$((n + 1))
  done
}

tmp="$(mktemp "${TMPDIR:-/tmp}/trinoodbc-postinstall.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
log "library built for $DRIVER_MANAGER: $DRIVER_PATH"

if [ "$DRIVER_MANAGER" = iodbc ]; then
  # ---- iODBC: /Library/ODBC/odbcinst.ini (same layout the Starburst driver's installer writes) ------
  mkdir -p "$ODBC_DIR"
  [ -f "$ODBCINST_INI" ] || : > "$ODBCINST_INI"
  ini_remove_section "$ODBCINST_INI" "$DRIVER_NAME"
  ini_remove_key "$ODBCINST_INI" "ODBC Drivers" "$DRIVER_NAME"
  ini_add_key "$ODBCINST_INI" "ODBC Drivers" "$DRIVER_NAME" "Installed"
  render_driver_section > "$tmp"
  ini_append_block "$ODBCINST_INI" < "$tmp"
  chmod 644 "$ODBCINST_INI"
  # iODBC's Administrator also creates odbc.ini next to it; make sure the directory is usable by it.
  [ -f "$ODBC_DIR/odbc.ini" ] || { : > "$ODBC_DIR/odbc.ini"; chmod 644 "$ODBC_DIR/odbc.ini"; }
  log "registered '$DRIVER_NAME' -> $DRIVER_PATH in $ODBCINST_INI (iODBC)"
  # Earlier packages also registered this iODBC build with unixODBC; undo exactly that registration
  # (a unixODBC build registered under the same name elsewhere is left alone).
  if [ -n "$ODBCINST_BIN" ] && [ "$(unixodbc_registered_path)" = "$DRIVER_PATH" ]; then
    unixodbc_unregister_all
    log "removed the stale unixODBC registration of this iODBC build ($ODBCINST_BIN)"
  fi
  if [ -n "$ODBCINST_BIN" ]; then
    log "note: unixODBC applications (isql, pyodbc, R) need the unixODBC package (build_pkg.sh --driver-manager unixodbc)"
  fi
else
  # ---- unixODBC (Homebrew): odbcinst -i -d ----------------------------------------------------------
  # A unixODBC build must never appear in iODBC's file; drop a registration of this very library.
  if [ "$(ini_registered_path "$ODBCINST_INI" "$DRIVER_NAME")" = "$DRIVER_PATH" ]; then
    ini_remove_section "$ODBCINST_INI" "$DRIVER_NAME"
    ini_remove_key "$ODBCINST_INI" "ODBC Drivers" "$DRIVER_NAME"
    log "removed a registration of this unixODBC build from $ODBCINST_INI (iODBC)"
  fi
  if [ -z "$ODBCINST_BIN" ]; then
    log "warning: unixODBC's odbcinst was not found (brew install unixodbc); the driver is installed but not registered."
    log "         register it later with: odbcinst -i -d -f <driver section>, Driver=$DRIVER_PATH (see docs/INSTALL-MACOS.md)"
  else
    unixodbc_unregister_all
    render_driver_section > "$tmp"
    if "$ODBCINST_BIN" -i -d -f "$tmp" >/dev/null 2>&1; then
      log "registered '$DRIVER_NAME' -> $DRIVER_PATH with unixODBC ($ODBCINST_BIN)"
    else
      log "warning: '$ODBCINST_BIN -i -d' failed; unixODBC registration skipped"
    fi
  fi
fi

rm -f "$tmp"
trap - EXIT
exit 0
