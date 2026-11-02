#!/bin/sh
# Builds the Trino ODBC driver on macOS and wraps it into a branded installer package (.pkg) and a
# disk image (.dmg) that carries the package and the documentation.
#
#   build_pkg.sh [options]
#
# Options:
#   --driver-manager DM     iodbc (default; the system driver manager) | unixodbc | both
#   --build-dir DIR         CMake build directory (default: build-macos-pkg, build-macos-pkg-unixodbc;
#                           with "both", DIR-iodbc and DIR-unixodbc)
#   --out DIR               output directory for the .pkg (default: dist/macos)
#   --version V             package version (default: from CMakeLists.txt)
#   --identifier ID         package identifier (default: io.github.ahmetfurkandemir.trinoodbc; the unixODBC package gets
#                           ID.unixodbc)
#   --sign-identity ID      codesign identity for the dylibs (default: "-" = ad-hoc)
#   --pkg-sign-identity ID  productsign "Developer ID Installer" identity (default: unsigned)
#   --no-bundle-deps        do not copy Homebrew dylib dependencies next to the driver
#   --no-dmg                do not build the .dmg (the .pkg only)
#   --skip-tests            do not build/run the unit tests
#   --jobs N                parallel build jobs (default: sysctl hw.ncpu)
#   --cmake-arg ARG         extra argument forwarded to cmake (repeatable)
#   -h, --help
#
# One package per driver manager, because SQLWCHAR (4 bytes with iODBC, 2 with unixODBC) and the
# installer library are fixed at compile time. Layout (installed under /; <dir> is TrinoODBC for the
# iODBC package and TrinoODBC-unixODBC for the unixODBC package, so both can be installed side by side):
#   /Library/ODBC/<dir>/lib/libtrinoodbc.dylib   (+ bundled dependencies, @loader_path)
#   /Library/ODBC/<dir>/share/odbcinst.ini.template, odbc.ini.example, VERSION, DRIVER_MANAGER
#   /Library/ODBC/<dir>/docs/*.md
# Output: dist/macos/TrinoODBC-<version>-macos-<arch>.pkg + .dmg (iODBC) and
#         dist/macos/TrinoODBC-<version>-macos-<arch>-unixodbc.pkg + .dmg (unixODBC).
# The .pkg is a product archive (productbuild + packaging/macos/distribution.xml.in) around the
# component package: Trino logo background, welcome pane (incl. the default session time zone
# Europe/Istanbul, UTC+03:00, and how to change it), license pane (LICENSE) and conclusion pane
# (installed files, DSN example); panes in English and Turkish (packaging/macos/resources/*.lproj).
# The .dmg (volume "Trino ODBC Driver", Trino volume icon) holds the .pkg, README.md and a
# Documentation folder.
# The postinstall script (packaging/macos/postinstall.sh) registers the library only with the driver
# manager it was built for: iODBC in /Library/ODBC/odbcinst.ini, unixODBC through Homebrew's
# odbcinst(1). Re-running the script overwrites the previous package.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DRIVER_MANAGER=iodbc
BUILD_DIR=""
OUT_DIR="$ROOT/dist/macos"
VERSION=""
IDENTIFIER="io.github.ahmetfurkandemir.trinoodbc"
SIGN_IDENTITY="-"
PKG_SIGN_IDENTITY=""
BUNDLE_DEPS=1
MAKE_DMG=1
SKIP_TESTS=0
JOBS=""
EXTRA_CMAKE_ARGS=""

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; }
log() { printf '%s\n' "build_pkg: $*"; }
die() { printf '%s\n' "build_pkg: error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --driver-manager) shift; DRIVER_MANAGER="$1" ;;
    --driver-manager=*) DRIVER_MANAGER="${1#*=}" ;;
    --build-dir) shift; BUILD_DIR="$1" ;;
    --build-dir=*) BUILD_DIR="${1#*=}" ;;
    --out) shift; OUT_DIR="$1" ;;
    --out=*) OUT_DIR="${1#*=}" ;;
    --version) shift; VERSION="$1" ;;
    --version=*) VERSION="${1#*=}" ;;
    --identifier) shift; IDENTIFIER="$1" ;;
    --identifier=*) IDENTIFIER="${1#*=}" ;;
    --sign-identity) shift; SIGN_IDENTITY="$1" ;;
    --sign-identity=*) SIGN_IDENTITY="${1#*=}" ;;
    --pkg-sign-identity) shift; PKG_SIGN_IDENTITY="$1" ;;
    --pkg-sign-identity=*) PKG_SIGN_IDENTITY="${1#*=}" ;;
    --no-bundle-deps) BUNDLE_DEPS=0 ;;
    --no-dmg) MAKE_DMG=0 ;;
    --skip-tests) SKIP_TESTS=1 ;;
    --jobs) shift; JOBS="$1" ;;
    --jobs=*) JOBS="${1#*=}" ;;
    --cmake-arg) shift; EXTRA_CMAKE_ARGS="$EXTRA_CMAKE_ARGS $1" ;;
    --cmake-arg=*) EXTRA_CMAKE_ARGS="$EXTRA_CMAKE_ARGS ${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option '$1' (see --help)" ;;
  esac
  shift
done

case "$DRIVER_MANAGER" in
  iodbc|unixodbc) FLAVOURS="$DRIVER_MANAGER" ;;
  both) FLAVOURS="iodbc unixodbc" ;;
  *) die "--driver-manager must be iodbc, unixodbc or both (got '$DRIVER_MANAGER')" ;;
esac
[ "$(uname -s)" = Darwin ] || die "this script must run on macOS"
for tool in cmake pkgbuild productbuild otool install_name_tool codesign; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool not found (install Xcode command line tools / cmake)"
done
if [ "$MAKE_DMG" = 1 ]; then
  command -v hdiutil >/dev/null 2>&1 || die "hdiutil not found (use --no-dmg)"
fi
RES_SRC="$SCRIPT_DIR/resources"
ASSETS="$ROOT/packaging/assets/generated"
[ -f "$SCRIPT_DIR/distribution.xml.in" ] || die "missing $SCRIPT_DIR/distribution.xml.in"
[ -f "$RES_SRC/en.lproj/welcome.html.in" ] || die "missing installer resources in $RES_SRC"
if [ -n "$BUILD_DIR" ]; then
  case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
fi
case "$OUT_DIR" in /*) ;; *) OUT_DIR="$ROOT/$OUT_DIR" ;; esac
if [ -z "$VERSION" ]; then
  VERSION="$(sed -n 's/^project(trinoodbc VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
  [ -n "$VERSION" ] || VERSION=1.0.0
fi
[ -n "$JOBS" ] || JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
ARCH="$(uname -m)"
log "version $VERSION, driver manager(s) $FLAVOURS, arch $ARCH"

sed_escape() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

# Detach a disk image left mounted by a failed DMG build.
DMG_MOUNT=""
cleanup() {
  if [ -n "$DMG_MOUNT" ] && [ -d "$DMG_MOUNT" ]; then
    hdiutil detach "$DMG_MOUNT" -quiet >/dev/null 2>&1 || hdiutil detach "$DMG_MOUNT" -force -quiet >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Renders one installer resource template (FILE.in -> FILE) with the per-package values below; a line
# <!--@STYLE@--> is replaced by packaging/macos/resources/style.css.
render_resource() {  # TEMPLATE OUTPUT
  sed -e "s|@VERSION@|$(sed_escape "$VERSION")|g" \
      -e "s|@ARCH@|$(sed_escape "$ARCH")|g" \
      -e "s|@TITLE@|$(sed_escape "$r_title")|g" \
      -e "s|@PKG_ID@|$(sed_escape "$pkg_id")|g" \
      -e "s|@COMPONENT_PKG@|$(sed_escape "$pkg_id.pkg")|g" \
      -e "s|@BACKGROUND@|$(sed_escape "$r_background")|g" \
      -e "s|@BACKGROUND_DARK@|$(sed_escape "$r_background_dark")|g" \
      -e "s|@INSTALL_DIR@|$(sed_escape "$install_dir")|g" \
      -e "s|@DRIVER_MANAGER_LABEL@|$(sed_escape "$r_dm_label")|g" \
      -e "s|@REGISTRATION@|$(sed_escape "$r_registration")|g" \
      -e "s|@REGISTRATION_TR@|$(sed_escape "$r_registration_tr")|g" \
      -e "s|@REGISTERED_TR@|$(sed_escape "$r_registered_tr")|g" \
      -e "s|@DRIVER_MANAGER_USERS@|$(sed_escape "$r_users")|g" \
      -e "s|@DRIVER_MANAGER_USERS_TR@|$(sed_escape "$r_users_tr")|g" \
      -e "s|@DSN_FILES@|$(sed_escape "$r_dsn_files")|g" \
      -e "s|@DSN_FILES_TR@|$(sed_escape "$r_dsn_files_tr")|g" \
      -e "s|@TEST_COMMAND@|$(sed_escape "$r_test")|g" \
      -e "s|@LOGO_DATA_URI@|$(sed_escape "$r_logo_uri")|g" \
      -e "/<!--@STYLE@-->/{" -e "r $RES_SRC/style.css" -e "d" -e "}" \
      "$1" > "$2"
  if grep -q '@[A-Z_]*@' "$2"; then
    die "unsubstituted placeholder in $2: $(grep -o '@[A-Z_]*@' "$2" | sort -u | tr '\n' ' ')"
  fi
}

# Installer background: one multi-resolution TIFF (1x + @2x) when tiffutil is available, else the PNG.
stage_background() {  # BASENAME (without .png) -> prints the file name placed in $res
  src="$RES_SRC/$1.png"
  src2="$RES_SRC/$1@2x.png"
  if [ ! -f "$src" ]; then  # fall back to the shared asset (logo centred on white)
    src="$ASSETS/pkg-background.png"
    src2="$ASSETS/pkg-background@2x.png"
  fi
  [ -f "$src" ] || die "no installer background ($RES_SRC/$1.png or $ASSETS/pkg-background.png)"
  if [ -f "$src2" ] && command -v tiffutil >/dev/null 2>&1 \
     && tiffutil -cathidpicheck "$src" "$src2" -out "$res/$1.tiff" >/dev/null 2>&1; then
    printf '%s' "$1.tiff"
  else
    cp -f "$src" "$res/$1.png"
    printf '%s' "$1.png"
  fi
}

# Wraps the product archive, README.md and the documentation into a compressed, read-only disk image.
make_dmg() {  # PKG VOLUME-NAME
  dmg="${1%.pkg}.dmg"
  dmg_stage="$build_dir/dmgroot"
  rw="$build_dir/dmg-rw.dmg"
  DMG_MOUNT="$build_dir/dmgmount"
  rm -rf "$dmg_stage" "$rw" "$DMG_MOUNT"
  mkdir -p "$dmg_stage/Documentation"
  cp -f "$1" "$dmg_stage/"
  [ -f "$ROOT/README.md" ] && cp -f "$ROOT/README.md" "$dmg_stage/README.md"
  [ -f "$ROOT/README.tr.md" ] && cp -f "$ROOT/README.tr.md" "$dmg_stage/Documentation/README.tr.md"
  find "$ROOT/docs" -maxdepth 1 -name '*.md' -exec cp -f {} "$dmg_stage/Documentation/" \;
  for f in LICENSE NOTICE THIRD-PARTY-NOTICES.md; do
    [ -f "$ROOT/$f" ] && cp -f "$ROOT/$f" "$dmg_stage/Documentation/$f"
  done
  have_icon=0
  if [ -f "$ASSETS/TrinoODBC.icns" ]; then
    cp -f "$ASSETS/TrinoODBC.icns" "$dmg_stage/.VolumeIcon.icns"
    have_icon=1
  else
    log "warning: $ASSETS/TrinoODBC.icns not found (packaging/assets/make_icons.sh); DMG without volume icon"
  fi
  # The DMG background image needs a Finder-written .DS_Store (window layout); it is not set here.
  hdiutil create -quiet -srcfolder "$dmg_stage" -volname "$2" -fs HFS+ -format UDRW -ov "$rw"
  if [ "$have_icon" = 1 ]; then
    # Custom-icon Finder flag on the volume root, so Finder shows .VolumeIcon.icns.
    mkdir -p "$DMG_MOUNT"
    hdiutil attach "$rw" -readwrite -noverify -noautoopen -nobrowse -quiet -mountpoint "$DMG_MOUNT"
    if command -v SetFile >/dev/null 2>&1; then
      SetFile -a C "$DMG_MOUNT"
      SetFile -a V "$DMG_MOUNT/.VolumeIcon.icns"
    else
      xattr -wx com.apple.FinderInfo \
        0000000000000000040000000000000000000000000000000000000000000000 "$DMG_MOUNT"
    fi
    rm -rf "$DMG_MOUNT/.fseventsd" 2>/dev/null || true
    hdiutil detach "$DMG_MOUNT" -quiet || hdiutil detach "$DMG_MOUNT" -force -quiet
    rmdir "$DMG_MOUNT" 2>/dev/null || true
  fi
  DMG_MOUNT=""
  rm -f "$dmg"
  hdiutil convert "$rw" -quiet -format UDZO -imagekey zlib-level=9 -o "$dmg"
  rm -f "$rw"
  rm -rf "$dmg_stage"
  if [ "$SIGN_IDENTITY" != "-" ]; then
    codesign --force --sign "$SIGN_IDENTITY" "$dmg"
  fi
  log "[$dm] disk image: $dmg"
}

is_external_dep() {
  case "$1" in
    /opt/homebrew/*|/usr/local/*|/opt/local/*) return 0 ;;
    *) return 1 ;;
  esac
}

bundle_deps() {  # FILE  (recursive; copies external dylibs next to the driver and rewrites load commands)
  target="$1"
  chmod u+w "$target"
  otool -L "$target" | awk 'NR > 1 { print $1 }' | while IFS= read -r dep; do
    is_external_dep "$dep" || continue
    # Resolve symlinks so the copied file is the real library and the name stays stable.
    real="$dep"
    [ -L "$real" ] && real="$(cd "$(dirname "$real")" && pwd -P)/$(readlink "$real")"
    [ -f "$real" ] || real="$dep"
    base="$(basename "$dep")"
    if [ ! -f "$LIB_DIR/$base" ]; then
      cp -f "$real" "$LIB_DIR/$base"
      chmod 755 "$LIB_DIR/$base"
      install_name_tool -id "@loader_path/$base" "$LIB_DIR/$base"
      # Subshell: POSIX sh has no local variables, and the recursion would clobber target/dep/base.
      ( bundle_deps "$LIB_DIR/$base" )
    fi
    install_name_tool -change "$dep" "@loader_path/$base" "$target"
  done
}

# Builds, stages and packages the driver for one driver manager (iodbc | unixodbc).
build_flavour() {
  dm="$1"
  if [ "$dm" = iodbc ]; then
    install_dir="/Library/ODBC/TrinoODBC"
    pkg_id="$IDENTIFIER"
    pkg_suffix=""
    default_build="$ROOT/build-macos-pkg"
  else
    install_dir="/Library/ODBC/TrinoODBC-unixODBC"
    pkg_id="$IDENTIFIER.unixodbc"
    pkg_suffix="-unixodbc"
    default_build="$ROOT/build-macos-pkg-unixodbc"
  fi
  if [ -z "$BUILD_DIR" ]; then
    build_dir="$default_build"
  elif [ "$FLAVOURS" = "$dm" ]; then
    build_dir="$BUILD_DIR"
  else
    build_dir="$BUILD_DIR-$dm"
  fi
  log "[$dm] building in $build_dir"

  # ---- build ------------------------------------------------------------------------------------------
  build_tests=ON
  [ "$SKIP_TESTS" = 1 ] && build_tests=OFF
  # shellcheck disable=SC2086  # EXTRA_CMAKE_ARGS is intentionally word-split
  cmake -S "$ROOT" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTRINO_ODBC_DRIVER_MANAGER="$dm" \
    -DTRINO_ODBC_BUILD_TESTS="$build_tests" \
    -DTRINO_ODBC_INSTALL_ODBCINST=OFF \
    $EXTRA_CMAKE_ARGS
  cmake --build "$build_dir" --config Release --parallel "$JOBS"
  # The configure step records the driver manager it really compiled against; a mismatch (e.g. an
  # --cmake-arg override) would ship a library that garbles wide strings under the registered DM.
  built_dm="$(tr -d ' \t\r\n' < "$build_dir/DRIVER_MANAGER" 2>/dev/null || true)"
  [ "$built_dm" = "$dm" ] || die "[$dm] the build in $build_dir targets driver manager '${built_dm:-unknown}', not '$dm'"
  dylib="$build_dir/libtrinoodbc.dylib"
  [ -f "$dylib" ] || dylib="$(find "$build_dir" -name libtrinoodbc.dylib -type f | head -n 1)"
  [ -n "$dylib" ] && [ -f "$dylib" ] || die "[$dm] libtrinoodbc.dylib was not built"

  if [ "$SKIP_TESTS" != 1 ] && [ -x "$build_dir/tests/trino_odbc_unit_tests" ]; then
    log "[$dm] running unit tests"
    "$build_dir/tests/trino_odbc_unit_tests" || die "[$dm] unit tests failed; not packaging"
  fi

  # ---- stage ------------------------------------------------------------------------------------------
  stage="$build_dir/pkgroot"
  scripts="$build_dir/pkgscripts"
  rm -rf "$stage" "$scripts"
  mkdir -p "$stage$install_dir/lib" "$stage$install_dir/share" "$stage$install_dir/docs" "$scripts"
  cp -f "$dylib" "$stage$install_dir/lib/libtrinoodbc.dylib"
  chmod 755 "$stage$install_dir/lib/libtrinoodbc.dylib"
  cp -f "$ROOT/src/platform/unix/odbcinst.ini.in" "$stage$install_dir/share/odbcinst.ini.template"
  cp -f "$ROOT/src/platform/unix/odbc.ini.example" "$stage$install_dir/share/odbc.ini.example"
  printf '%s\n' "$VERSION" > "$stage$install_dir/share/VERSION"
  printf '%s\n' "$dm" > "$stage$install_dir/share/DRIVER_MANAGER"
  if [ -d "$ROOT/docs" ]; then
    find "$ROOT/docs" -maxdepth 1 -name '*.md' -exec cp -f {} "$stage$install_dir/docs/" \;
  fi
  [ -f "$ROOT/README.md" ] && cp -f "$ROOT/README.md" "$stage$install_dir/docs/README.md"
  [ -f "$ROOT/LICENSE" ] && cp -f "$ROOT/LICENSE" "$stage$install_dir/docs/LICENSE"
  [ -f "$ROOT/NOTICE" ] && cp -f "$ROOT/NOTICE" "$stage$install_dir/docs/NOTICE"
  [ -f "$ROOT/THIRD-PARTY-NOTICES.md" ] && cp -f "$ROOT/THIRD-PARTY-NOTICES.md" "$stage$install_dir/docs/THIRD-PARTY-NOTICES.md"
  # The postinstall script learns which driver manager (and where) it registers for from the package.
  sed -e "s|@TRINO_ODBC_DRIVER_MANAGER@|$(sed_escape "$dm")|g" \
      -e "s|@TRINO_ODBC_INSTALL_DIR@|$(sed_escape "$install_dir")|g" \
      "$SCRIPT_DIR/postinstall.sh" > "$scripts/postinstall"
  chmod 755 "$scripts/postinstall"

  # ---- bundle Homebrew dependencies (so the driver loads on machines without Homebrew) ---------------
  LIB_DIR="$stage$install_dir/lib"
  if [ "$BUNDLE_DEPS" = 1 ]; then
    log "[$dm] bundling external dylib dependencies"
    bundle_deps "$LIB_DIR/libtrinoodbc.dylib"
  fi

  # install_name_tool invalidates signatures: (re)sign every library, ad-hoc unless an identity was given.
  for lib in "$LIB_DIR"/*.dylib; do
    codesign --force --sign "$SIGN_IDENTITY" --timestamp=none "$lib" >/dev/null 2>&1 \
      || codesign --force --sign "$SIGN_IDENTITY" "$lib"
  done
  log "[$dm] libraries in package:"
  ls -1 "$LIB_DIR" | sed 's/^/  /'
  otool -L "$LIB_DIR/libtrinoodbc.dylib" | awk 'NR > 1 { print "  dep: " $1 }'

  # ---- package ----------------------------------------------------------------------------------------
  # Component package (payload + postinstall), then the product archive with the installer panes.
  mkdir -p "$OUT_DIR"
  pkg="$OUT_DIR/TrinoODBC-$VERSION-macos-$ARCH$pkg_suffix.pkg"
  comp="$build_dir/pkgcomponent"
  res="$build_dir/pkgresources"
  rm -rf "$comp" "$res"
  mkdir -p "$comp" "$res"
  pkgbuild --root "$stage" \
    --identifier "$pkg_id" \
    --version "$VERSION" \
    --install-location / \
    --scripts "$scripts" \
    "$comp/$pkg_id.pkg"

  if [ "$dm" = iodbc ]; then
    r_title="Trino ODBC Driver"
    r_dm_label="iODBC"
    r_registration="in <code>/Library/ODBC/odbcinst.ini</code> (iODBC)"
    r_registration_tr="iODBC'nin <code>/Library/ODBC/odbcinst.ini</code> dosyasına kaydeder"
    r_registered_tr="iODBC'nin <code>/Library/ODBC/odbcinst.ini</code> dosyasına kaydedildi"
    r_users="iODBC is the driver manager of Excel for Mac, Tableau and most other graphical applications. Command-line tools such as <code>isql</code>, pyodbc and R use unixODBC and need the unixODBC package of this driver."
    r_users_tr="iODBC; Excel for Mac, Tableau ve diğer grafik uygulamaların çoğunun sürücü yöneticisidir. <code>isql</code>, pyodbc ve R gibi komut satırı araçları unixODBC kullanır ve bu sürücünün unixODBC paketini gerektirir."
    r_dsn_files="<code>/Library/ODBC/odbc.ini</code> (all users) or <code>~/Library/ODBC/odbc.ini</code> (your user only)"
    r_dsn_files_tr="<code>/Library/ODBC/odbc.ini</code> (tüm kullanıcılar) ya da <code>~/Library/ODBC/odbc.ini</code> (yalnızca sizin kullanıcınız) dosyasına"
    r_test='iodbctest "DSN=TrinoProd"'
    volname="Trino ODBC Driver"
  else
    r_title="Trino ODBC Driver (unixODBC)"
    r_dm_label="unixODBC"
    r_registration="with Homebrew unixODBC (<code>odbcinst -i -d</code>; without <code>brew install unixodbc</code> the driver is installed but not registered)"
    r_registration_tr="Homebrew unixODBC'ye kaydeder (<code>odbcinst -i -d</code>; <code>brew install unixodbc</code> yapılmamışsa sürücü kurulur ama kaydedilmez)"
    r_registered_tr="Homebrew unixODBC'ye kaydedildi (<code>odbcinst -i -d</code>; <code>odbcinst</code> bulunamadıysa sürücü kurulmuş ama kaydedilmemiştir)"
    r_users="unixODBC is used by <code>isql</code>, pyodbc, R and most command-line tools. Graphical applications such as Excel for Mac use iODBC and need the iODBC package of this driver."
    r_users_tr="unixODBC; <code>isql</code>, pyodbc, R ve komut satırı araçlarının çoğu tarafından kullanılır. Excel for Mac gibi grafik uygulamalar iODBC kullanır ve bu sürücünün iODBC paketini gerektirir."
    r_dsn_files="<code>~/.odbc.ini</code> or <code>\$(brew --prefix)/etc/odbc.ini</code>"
    r_dsn_files_tr="<code>~/.odbc.ini</code> ya da <code>\$(brew --prefix)/etc/odbc.ini</code> dosyasına"
    r_test="isql -v TrinoProd"
    volname="Trino ODBC Driver (unixODBC)"
  fi
  r_background="$(stage_background background)"
  r_background_dark="$(stage_background background-dark)"
  # productbuild only archives the resources the Distribution names (and their localizations), so
  # the panes are self-contained: style.css is inlined and the logo is embedded as a data: URI.
  logo_png="$RES_SRC/logo.png"
  [ -f "$logo_png" ] || logo_png="$ASSETS/png/logo-256.png"
  [ -f "$logo_png" ] || die "no logo for the installer panes ($RES_SRC/logo.png or $ASSETS/png/logo-256.png)"
  r_logo_uri="data:image/png;base64,$(base64 < "$logo_png" | tr -d '\n')"
  [ -f "$ROOT/LICENSE" ] || die "missing $ROOT/LICENSE (license pane)"
  cp -f "$ROOT/LICENSE" "$res/LICENSE.txt"
  for lproj in "$RES_SRC"/*.lproj; do
    lname="$(basename "$lproj")"
    mkdir -p "$res/$lname"
    for tmpl in "$lproj"/*.html.in; do
      render_resource "$tmpl" "$res/$lname/$(basename "$tmpl" .in)"
    done
  done
  render_resource "$SCRIPT_DIR/distribution.xml.in" "$build_dir/distribution.xml"

  rm -f "$pkg"
  productbuild --distribution "$build_dir/distribution.xml" \
    --resources "$res" \
    --package-path "$comp" \
    --identifier "$pkg_id.product" \
    --version "$VERSION" \
    "$pkg"
  if [ -n "$PKG_SIGN_IDENTITY" ]; then
    command -v productsign >/dev/null 2>&1 || die "productsign not found"
    productsign --sign "$PKG_SIGN_IDENTITY" "$pkg" "$pkg.signed"
    mv -f "$pkg.signed" "$pkg"
  fi
  log "[$dm] package: $pkg"
  log "[$dm] install with: sudo installer -pkg \"$pkg\" -target /   (or open it in Installer.app)"
  if [ "$MAKE_DMG" = 1 ]; then
    make_dmg "$pkg" "$volname"
  fi
}

for flavour in $FLAVOURS; do
  build_flavour "$flavour"
done
