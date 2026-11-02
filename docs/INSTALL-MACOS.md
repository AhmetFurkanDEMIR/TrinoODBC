# Installing on macOS

## Two driver managers

macOS applications use one of two ODBC driver managers, and **the driver must be built for the
one your application uses**. Their wide-character type differs: `SQLWCHAR` is 4 bytes with iODBC
and 2 bytes with unixODBC. As a consequence, `SQL_DESC_OCTET_LENGTH` of a wide character column
is 4 x its length with iODBC.

| Driver manager | Used by | Configuration files |
|---|---|---|
| **iODBC** (default for this driver on macOS) | Excel for Mac, Tableau, most GUI applications, `iodbctest` | `/Library/ODBC/odbcinst.ini`, `/Library/ODBC/odbc.ini` (system) and `~/Library/ODBC/odbcinst.ini`, `~/Library/ODBC/odbc.ini` (user); also `~/.odbcinst.ini` / `~/.odbc.ini`, `$ODBCINSTINI` / `$ODBCINI` |
| **unixODBC** (`brew install unixodbc`) | `isql`, Python `pyodbc`, R, most command-line tools | `$(brew --prefix)/etc/odbcinst.ini`, `$(brew --prefix)/etc/odbc.ini`, `~/.odbc.ini`; `odbcinst -j` prints them |

A wrong pairing does not crash, but every wide string comes back garbled.

## Option A: the package

Ready-built packages for Apple silicon are on the
[Releases page](https://github.com/AhmetFurkanDEMIR/TrinoODBC/releases) (`TrinoODBC-<version>-macos-arm64.pkg`
and `.dmg`, with a `SHA256SUMS` file). A released package loads only on the macOS version it was
built on or newer, because the Homebrew libraries bundled in it carry that minimum version (the
1.0.0 package: macOS 26); on an older macOS or an Intel Mac, build the package yourself as
described here.

`packaging/macos/build_pkg.sh` builds the driver, runs the unit tests and wraps the result in an
installer package (`.pkg`) and a disk image (`.dmg`) that carries the package. Because the wide-character width and the installer library are fixed at compile
time, there is **one package per driver manager**, and each package registers its library only with
the driver manager it was built for:

| `--driver-manager` | Package and disk image (`<arch>` is `uname -m`, e.g. `arm64`) | Installs to | Registers with |
|---|---|---|---|
| `iodbc` (default) | `dist/macos/TrinoODBC-<version>-macos-<arch>.pkg` and `.dmg` | `/Library/ODBC/TrinoODBC/` | iODBC: `/Library/ODBC/odbcinst.ini` |
| `unixodbc` | `dist/macos/TrinoODBC-<version>-macos-<arch>-unixodbc.pkg` and `.dmg` | `/Library/ODBC/TrinoODBC-unixODBC/` | Homebrew unixODBC, through its `odbcinst -i -d` |
| `both` | all of the above (separate build directories) | | |

```sh
packaging/macos/build_pkg.sh                             # iODBC package
packaging/macos/build_pkg.sh --driver-manager unixodbc   # unixODBC package (needs brew install unixodbc)
packaging/macos/build_pkg.sh --driver-manager both
sudo installer -pkg dist/macos/TrinoODBC-1.0.0-macos-arm64.pkg -target /   # or double-click the .pkg
```

### The installer

Opened in Installer.app (double-click the `.pkg`, or open the `.dmg` and double-click the package
inside it), the package shows the Trino logo in the sidebar and these pages (in English, or in
Turkish on a Mac set to Turkish):

1. **Introduction**: what is installed, where, and for which driver manager, and a highlighted box
   with the **default session time zone: Europe/Istanbul (Turkey, UTC+03:00)** and how to change it
   (`TimeZone` in the DSN, see [Time zone](#time-zone) below).
2. **License**: the Apache License 2.0 (`LICENSE`).
3. **Installation Type / Installation**: installs on the system volume (the driver goes to
   `/Library/ODBC`); macOS asks for an administrator password.
4. **Summary**: the installed files and an `odbc.ini` DSN example (including the commented
   `TimeZone` line) with the command to test it.

macOS Installer has no text field for a setting such as the time zone (that would need an
Installer plug-in), and the time zone is a per-DSN setting anyway, so the installer states the
default and shows how to change it instead of asking for it. `sudo installer -pkg ...` installs
the same package without any of these pages.

### The disk image

The `.dmg` (volume name *Trino ODBC Driver*, with the Trino logo as volume icon; the unixODBC
image is called *Trino ODBC Driver (unixODBC)*) contains the `.pkg`, `README.md` and a
`Documentation` folder with these documents, `LICENSE`, `NOTICE` and `THIRD-PARTY-NOTICES.md`.
It is a compressed, read-only image (UDZO); verify it with `hdiutil verify`. It has no background
picture or window layout (that needs a Finder-written `.DS_Store`). `--no-dmg` builds the `.pkg`
only.

The installer artwork lives in `packaging/macos/resources/` (background images, the English and
Turkish pane templates, their style sheet) and `packaging/macos/distribution.xml.in`;
`packaging/macos/make_backgrounds.sh` regenerates the images from the Trino logo, see
[packaging/assets/README.md](../packaging/assets/README.md).

The two packages install to different directories and can be installed side by side, so GUI
applications (iODBC) and `isql` / `pyodbc` / R (unixODBC) can both use the driver.

Options: `--driver-manager iodbc|unixodbc|both`, `--build-dir DIR` (default `build-macos-pkg`,
`build-macos-pkg-unixodbc`; with `both`, `DIR-iodbc` and `DIR-unixodbc`), `--out DIR` (default
`dist/macos`), `--version V`, `--identifier ID` (default `io.github.ahmetfurkandemir.trinoodbc`; the unixODBC package uses
`ID.unixodbc`), `--sign-identity ID` (codesign identity for the dylibs, default ad-hoc `-`),
`--pkg-sign-identity ID` (productsign *Developer ID Installer* identity for the `.pkg`, default
unsigned; with a real `--sign-identity`, the `.dmg` is also codesigned), `--no-bundle-deps`,
`--no-dmg`, `--skip-tests`, `--jobs N`, `--cmake-arg ARG` (repeatable) and `--help`.
The script refuses to package a build whose configured driver manager (recorded by CMake in
`<build>/DRIVER_MANAGER`) differs from the requested one.

Each package installs (`<dir>` is `TrinoODBC` or `TrinoODBC-unixODBC`):

* `/Library/ODBC/<dir>/lib/libtrinoodbc.dylib`, plus copies of the Homebrew libraries it links
  (`libiodbcinst` or `libodbcinst`, `libsimdjson`, `libzstd`, `liblz4`, ...). Their load commands
  are rewritten to `@loader_path`, so the driver also loads on a Mac without Homebrew
  (`--no-bundle-deps` turns this off). Every dylib is re-signed.
* `/Library/ODBC/<dir>/share/`: `odbcinst.ini.template`, `odbc.ini.example`, `VERSION` and
  `DRIVER_MANAGER` (the driver manager the library was built for).
* `/Library/ODBC/<dir>/docs/`: the Markdown documentation.

The postinstall script then registers *Trino ODBC Driver*:

* iODBC package: in `/Library/ODBC/odbcinst.ini` (iODBC format, the same layout the Starburst
  driver uses), creating an empty `/Library/ODBC/odbc.ini` if there is none. It never registers the
  iODBC build with unixODBC, and removes such a registration left behind by an older package.
* unixODBC package: with `odbcinst -i -d` from Homebrew unixODBC (`/opt/homebrew/bin`,
  `/usr/local/bin` or `PATH`). Without `odbcinst` the library is installed but not registered (the
  installer log says so; see [unixODBC](#unixodbc) below to register it by hand).

Re-installing or upgrading leaves exactly one registration per driver manager. A package downloaded
from a browser may carry the quarantine attribute. If an application refuses to load the library,
run `sudo xattr -dr com.apple.quarantine /Library/ODBC/TrinoODBC` (or `TrinoODBC-unixODBC`).

## Option B: build from source

```sh
xcode-select --install                       # compiler, SDK (includes the system libcurl)
brew install cmake ninja simdjson zstd lz4
brew install libiodbc                        # for iODBC builds (headers + libiodbcinst)
brew install unixodbc                        # for unixODBC builds (either or both)

git clone https://github.com/AhmetFurkanDEMIR/TrinoODBC.git trino-odbc && cd trino-odbc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRINO_ODBC_DRIVER_MANAGER=iodbc
cmake --build build
ctest --test-dir build                       # unit tests + integration tests against the mock server (python3)
```

`-DTRINO_ODBC_DRIVER_MANAGER=unixodbc` builds for unixODBC (`auto` picks iODBC on macOS). The
CMake helper (`cmake/TrinoOdbcHelpers.cmake`) searches the Homebrew prefixes `/opt/homebrew/opt/<formula>`
(Apple Silicon) and `/usr/local/opt/<formula>` (Intel) for the driver manager (`libiodbc` or
`unixodbc`), simdjson, zstd and lz4. For iODBC it needs `odbcinst.h` and `libiodbcinst`, which
Homebrew's `libiodbc` provides. It prefers real header directories over the `iODBC.framework`
bundle. If simdjson is not installed, it is fetched and built statically. zstd and lz4 are
optional (`auto`). libcurl is found with CMake's standard `find_package(CURL)`, which picks the
**macOS system libcurl** (`/usr/lib/libcurl.4.dylib`). To link Homebrew's curl instead, add
`-DCMAKE_PREFIX_PATH=$(brew --prefix curl)`. The other CMake options are listed in
[INSTALL-LINUX.md](INSTALL-LINUX.md#option-b-build-from-source).

The result is `build/libtrinoodbc.dylib` (install name `@rpath/libtrinoodbc.dylib`). `otool -L
build/libtrinoodbc.dylib` shows its dependencies.

Install it:

```sh
sudo cmake --install build --prefix /usr/local     # -> /usr/local/lib/libtrinoodbc.dylib
                                                   #    + share/trinoodbc/odbcinst.ini.template, share/doc/trinoodbc/
# or
sudo mkdir -p /Library/ODBC/TrinoODBC/lib && sudo cp build/libtrinoodbc.dylib /Library/ODBC/TrinoODBC/lib/
```

A driver installed this way still loads its dependencies from Homebrew. Use the package (Option A)
for machines without Homebrew.

Kerberos needs a libcurl with SPNEGO. The system libcurl the default build links has it
(`/usr/bin/curl --version` lists `GSS-API`, `Kerberos` and `SPNEGO` under *Features*).

## Registering the driver

### iODBC

Append to `/Library/ODBC/odbcinst.ini` (system) or `~/Library/ODBC/odbcinst.ini` (user), and
create the file when it does not exist:

```ini
[ODBC Drivers]
Trino ODBC Driver=Installed

[Trino ODBC Driver]
Description=Trino ODBC Driver 1.0.0 (ODBC 3.8, Unicode)
Driver=/Library/ODBC/TrinoODBC/lib/libtrinoodbc.dylib
Setup=/Library/ODBC/TrinoODBC/lib/libtrinoodbc.dylib
APILevel=2
ConnectFunctions=YYN
DriverODBCVer=03.80
FileUsage=0
SQLLevel=1
UsageCount=1
```

`src/platform/unix/odbcinst.ini.in` is the same text with placeholders. The Linux registration
script also edits iODBC's file correctly when it is given the file explicitly. `--odbcinst`
bypasses `odbcinst(1)`, and running the script again does not duplicate the entry:

```sh
sudo packaging/linux/register_driver.sh --odbcinst /Library/ODBC/odbcinst.ini /Library/ODBC/TrinoODBC/lib/libtrinoodbc.dylib
sudo packaging/linux/register_driver.sh --odbcinst /Library/ODBC/odbcinst.ini --unregister
```

### unixODBC

```sh
sed -e 's|@TRINO_ODBC_DRIVER_NAME@|Trino ODBC Driver|' \
    -e 's|@TRINO_ODBC_DRIVER_PATH@|/usr/local/lib/libtrinoodbc.dylib|' \
    -e 's|@TRINO_ODBC_VERSION@|1.0.0|' \
    -e '/^;/d' -e '/^\[ODBC Drivers\]/,/^$/d' src/platform/unix/odbcinst.ini.in > /tmp/trino-driver.ini
odbcinst -i -d -f /tmp/trino-driver.ini       # Homebrew's etc/ is writable by the user; no sudo needed
odbcinst -q -d -n "Trino ODBC Driver"
```

`packaging/linux/register_driver.sh /usr/local/lib/libtrinoodbc.dylib` does the same when
Homebrew's `odbcinst` is on `PATH`. Register only a library built with
`-DTRINO_ODBC_DRIVER_MANAGER=unixodbc` this way (`share/DRIVER_MANAGER` in the package, or
`<build>/DRIVER_MANAGER`, names the driver manager a build targets); an iODBC build registered with
unixODBC garbles every wide string.

## Creating a DSN

iODBC: `~/Library/ODBC/odbc.ini` (user) or `/Library/ODBC/odbc.ini` (system). unixODBC:
`~/.odbc.ini` or `$(brew --prefix)/etc/odbc.ini`. The format is the same:

```ini
[ODBC Data Sources]
TrinoProd=Trino ODBC Driver

[TrinoProd]
Driver=Trino ODBC Driver
Host=trino.corp.example.com
Port=8443
AuthenticationType=LDAP
UID=jdoe
SSL=1
TrustedCerts=/Users/jdoe/certs/corp-root-ca.pem
Catalog=hive
Schema=default
```

With iODBC, the `Driver=` line may also be the absolute path of the dylib.

### Time zone

The driver runs the Trino session in **Europe/Istanbul (Turkey, UTC+03:00)** unless the DSN says
otherwise; this is the project default, and the installer's first page states it. To change it,
add one of these lines to the DSN (or the connection string):

```ini
TimeZone=Europe/Berlin               ; any IANA zone name, or a fixed offset such as +05:30
TimeZone=system                      ; the time zone of this Mac
TimestampWithTimeZoneMode=utc        ; timestamp with time zone values in UTC
```

Details: [CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md#default-time-zone-europeistanbul).
`src/platform/unix/odbc.ini.example` contains a DSN for every authentication type. The macOS
*ODBC Manager* / *iODBC Administrator* applications (when installed) edit the same files.

Without an explicit `SSL=` key, TLS is enabled for port 443 and whenever credentials or an
authentication type are configured. `SSL=0` or `AllowInsecurePassword=1` turns that off; see
[INSTALL-LINUX.md](INSTALL-LINUX.md#creating-a-dsn) for the full rule.

## Verifying

iODBC (`iodbctest` / `iodbctestw` come with the iODBC SDK in `/Library/Application Support/iODBC/bin/`
and with Homebrew's keg-only `libiodbc` in `$(brew --prefix libiodbc)/bin/`):

```sh
iodbctest "DSN=TrinoProd;PWD=secret"          # then: SQL>SELECT version();
iodbctestw "DSN=TrinoProd;PWD=secret"         # wide-character API
```

unixODBC (with a driver built for unixODBC):

```sh
isql -v TrinoProd jdoe secret
```

Python (`pip install pyodbc`; pyodbc uses unixODBC, so it needs `brew install unixodbc` and a
driver built with `-DTRINO_ODBC_DRIVER_MANAGER=unixodbc`):

```python
import pyodbc
c = pyodbc.connect("DSN=TrinoProd;PWD=secret")
print(c.execute("SELECT version(), current_user").fetchone())
```

When `LogLevel` or `TRINO_ODBC_LOG_LEVEL` is set, the log file is `$TMPDIR/trino_odbc_<pid>.log`.
`$TMPDIR` is the per-user temporary directory (`/var/folders/...`). `LogPath` /
`TRINO_ODBC_LOG_PATH` select another file or directory.

## Benchmarking

`trino_odbc_bench` is built with the project (`cmake --build build --target trino_odbc_bench`, output
`build/trino_odbc_bench`). It loads the dylib directly, so it works with either driver-manager
build and needs no DSN. Options and output are described in
[INSTALL-LINUX.md](INSTALL-LINUX.md#benchmarking).

```sh
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.dylib;Host=localhost;Port=18080;UID=test" \
    --sql "SELECT * FROM tpch.tiny.orders"
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.dylib;Host=localhost;Port=18443;SSLVerification=None;UID=alice;PWD=secret"
```

The second line (default query `SELECT * FROM tpch.sf1.orders`, TLS enabled automatically because
of the password) uses the containers of [tests/docker/README.md](../tests/docker/README.md). With
that setup on an Apple M-series laptop, the 1.5M rows of `tpch.sf1.orders` were fetched at
1.5-2.6M rows/s over TLS with the spooled protocol. The classic protocol over plain HTTP reached
about 300k rows/s.

## Excel for Mac

Excel uses iODBC and looks for system DSNs in `/Library/ODBC/odbc.ini`. *Data > Get Data (Power
Query) > From Database > From ODBC* (or the legacy *From Database* dialog) lists the DSN; enter
the user name and password when asked. Excel runs the query on a background thread, so use a
`RequestTimeout`/`QueryTimeout` suited to your queries.

## Common problems

| Symptom | Fix |
|---|---|
| `[iODBC][Driver Manager]Specified driver could not be loaded` | Wrong path in `odbcinst.ini`, quarantine attribute, or an Intel/Apple Silicon mismatch (`file libtrinoodbc.dylib`). |
| `iODBC not found (brew install libiodbc, ...)` at configure time | `brew install libiodbc`, or configure with `-DTRINO_ODBC_DRIVER_MANAGER=unixodbc`. |
| Garbled or truncated text, every second character missing | Driver built for the other driver manager; rebuild with the matching `TRINO_ODBC_DRIVER_MANAGER`. |
| `Library not loaded: .../libzstd...` / `libsimdjson` | Homebrew libraries moved or uninstalled for a source build; `otool -L libtrinoodbc.dylib` shows what is expected. The package bundles them. |
| DSN not found from a GUI application although `isql` works | The GUI application uses iODBC and reads `~/Library/ODBC/odbc.ini`, not unixODBC's files. |

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for connection and query errors.
