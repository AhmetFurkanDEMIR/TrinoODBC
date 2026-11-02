# Installing on Linux

The driver is built against **unixODBC** by default (`TRINO_ODBC_DRIVER_MANAGER=auto` selects
`unixodbc` on Linux). An iODBC build is possible with `-DTRINO_ODBC_DRIVER_MANAGER=iodbc`. The
two builds are not interchangeable, because iODBC uses a 4-byte `SQLWCHAR` and unixODBC a 2-byte one.

## Requirements

Runtime:

* unixODBC 2.3+ (on Debian/Ubuntu the `odbcinst` tool is a separate package, `odbcinst`; `isql` /
  `iusql` are in `unixodbc`),
* libcurl (OpenSSL or GnuTLS build),
* libzstd and liblz4 when the driver was built with them (compressed spooled encodings),
* the shared simdjson library when the build used the distribution's `libsimdjson-dev`,
* the IANA time-zone database (`tzdata`, `/usr/share/zoneinfo`), which the driver reads directly.

Build: a C++17 compiler (GCC 9+ / Clang 10+), CMake 3.16+ (3.20+ for `ctest --test-dir`), Ninja
or Make, and the development packages:

```sh
# Debian / Ubuntu (the package set the Ubuntu 24.04 test image uses)
sudo apt-get install build-essential cmake ninja-build git unixodbc unixodbc-dev odbcinst \
                     libcurl4-openssl-dev libzstd-dev liblz4-dev libsimdjson-dev python3 tzdata

# RHEL / Rocky / Fedora (Fedora also has simdjson-devel)
sudo dnf install gcc-c++ cmake ninja-build git unixODBC unixODBC-devel libcurl-devel libzstd-devel lz4-devel python3 tzdata
```

When no system simdjson is found (a CMake package config), simdjson `v3.13.0` is fetched with
`FetchContent` and linked statically (`TRINO_ODBC_SIMDJSON_VERSION` selects another tag). The
driver then does not depend on a shared simdjson at run time. `python3` is only needed for the
integration tests (mock Trino server).

## Option A: packages

Ready-built packages are on the [Releases page](https://github.com/AhmetFurkanDEMIR/TrinoODBC/releases):
`.deb` for Debian/Ubuntu (`amd64`, `arm64`), `.rpm` for RHEL/Fedora/SUSE (`x86_64`, `aarch64`) and
`.tar.gz` for other distributions, with a `SHA256SUMS` file. They are built on Ubuntu 24.04 (glibc
2.39) against unixODBC and libcurl; on an older distribution build the package yourself as
described here.

`packaging/linux/build_packages.sh` configures and builds the driver (default
`-DTRINO_ODBC_DRIVER_MANAGER=unixodbc`, install prefix `/usr`), runs the unit tests and packages
the result with CPack:

```sh
packaging/linux/build_packages.sh                  # -> dist/linux/
```

It always writes a `.tar.gz` (`trino-odbc-<version>-linux-<arch>.tar.gz`). It also writes a
`.deb` (`trino-odbc_<version>_<arch>.deb`) when `dpkg-deb` is available and an `.rpm`
(`trino-odbc-<version>-1.<arch>.rpm`) when `rpmbuild` is available. The library goes to the
distribution's library directory: `/usr/lib/<multiarch>` on Debian/Ubuntu (for example
`/usr/lib/x86_64-linux-gnu/libtrinoodbc.so`), `/usr/lib64` on 64-bit RPM systems, `/usr/lib`
otherwise.

Options: `--build-dir DIR` (default `build-linux-pkg`), `--out DIR` (default `dist/linux`),
`--generators "TGZ;DEB;RPM"`, `--driver-manager unixodbc|iodbc`, `--prefix DIR`, `--libdir NAME`,
`--version V`, `--jobs N`, `--skip-tests`, `--cmake-arg ARG` (repeatable) and `--help`.

The deb/rpm maintainer scripts embed `packaging/linux/register_driver.sh`. Installing a package
therefore **registers the driver** as *Trino ODBC Driver* (with `odbcinst -i -d`, or by editing
`/etc/odbcinst.ini` when `odbcinst` is missing), and removing it unregisters the driver. An
upgrade keeps the registration.

```sh
sudo apt install ./dist/linux/trino-odbc_1.0.0_amd64.deb   # or: sudo dnf install ./dist/linux/trino-odbc-1.0.0-1.x86_64.rpm
odbcinst -q -d -n "Trino ODBC Driver"                       # shows the registration
```

The `.deb` declares only `unixodbc, libcurl4` as dependencies (no automatic shared-library
dependencies). `libzstd1`, `liblz4-1` and, for a build against the system simdjson, the matching
`libsimdjson` runtime package must already be installed. To make the package independent of the
system simdjson, build it with
`--cmake-arg -DTRINO_ODBC_USE_SYSTEM_SIMDJSON=OFF`. The `.tar.gz` is not registered anywhere; run
`register_driver.sh` after unpacking it.

There is no graphical installer on Linux (the Windows and macOS installers show the default time
zone during installation); the packages install and register the driver without asking anything.
The session time zone defaults to **Europe/Istanbul (Turkey, UTC+03:00)** and is set per DSN with
`TimeZone` in `odbc.ini`, see [Creating a DSN](#creating-a-dsn).

## Option B: build from source

```sh
git clone https://github.com/AhmetFurkanDEMIR/TrinoODBC.git trino-odbc && cd trino-odbc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRINO_ODBC_DRIVER_MANAGER=unixodbc
cmake --build build
ctest --test-dir build                     # unit tests + integration tests against the mock server
sudo cmake --install build --prefix /usr/local
```

`cmake --install` installs:

* `lib/libtrinoodbc.so` (GNUInstallDirs: `/usr/local/lib` on Debian/Ubuntu, `/usr/local/lib64` on
  64-bit RPM systems),
* `share/trinoodbc/odbcinst.ini.template` (a copy of `src/platform/unix/odbcinst.ini.in`),
* the Markdown files of `docs/` under `share/doc/trinoodbc/`.

The targets are `trinoodbc` (the driver, `build/libtrinoodbc.so`), `trinoodbc_core` (the static
core it is linked from), `trino_odbc_bench` (see [Benchmarking](#benchmarking)) and, with tests,
`trino_odbc_unit_tests` and `trino_odbc_integration_tests` (under `build/tests/`).

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `TRINO_ODBC_DRIVER_MANAGER` | `auto` | `auto` / `unixodbc` / `iodbc` / `windows`. The unixODBC headers and `libodbcinst` are searched in `/usr/include`, `/usr/lib`, `/usr/lib64` and `/usr/lib/<multiarch>`. |
| `TRINO_ODBC_WITH_ZSTD`, `TRINO_ODBC_WITH_LZ4` | `auto` | `auto` uses the library when found; `ON` fails the configure step when it is missing; `OFF` builds without it (`json` encoding only). |
| `TRINO_ODBC_USE_SYSTEM_SIMDJSON` | `auto` | `auto` / `ON` (fail when missing) / `OFF` (always FetchContent, static). |
| `TRINO_ODBC_BUILD_TESTS` | `ON` | unit and integration tests. |
| `TRINO_ODBC_WERROR` | `OFF` | warnings are errors. |
| `TRINO_ODBC_INSTALL_ODBCINST` | `OFF` | `cmake --install` also runs `packaging/linux/register_driver.sh <prefix>/<libdir>/libtrinoodbc.so`. |

The configure output ends with a line such as `Trino ODBC: driver manager=unixodbc zstd=TRUE
lz4=TRUE simdjson=system`.

With `TRINO_ODBC_INSTALL_ODBCINST=ON`, the registered path is built from the
`CMAKE_INSTALL_PREFIX` of the *configure* step. Pass `-DCMAKE_INSTALL_PREFIX=/usr/local` when
configuring rather than `--prefix` at install time, otherwise the registration can point at the
wrong directory.

`ldd build/libtrinoodbc.so` shows the shared libraries the driver needs at run time.

## Registering the driver

```sh
sudo packaging/linux/register_driver.sh /usr/local/lib/libtrinoodbc.so
```

The script renders `src/platform/unix/odbcinst.ini.in` (or the installed
`share/trinoodbc/odbcinst.ini.template`) and runs `odbcinst -i -d`. When `odbcinst` is not
available, it edits `$ODBCSYSINI/odbcinst.ini` or `/etc/odbcinst.ini` directly. It is idempotent:
it replaces a registration that points at another path and leaves exactly one. Options:

| Option | Meaning |
|---|---|
| `--unregister` | remove the registration |
| `--name NAME` | driver name (default `Trino ODBC Driver`) |
| `--odbcinst FILE` | edit this INI file directly instead of calling `odbcinst` (default `$ODBCINSTINI`) |
| `--no-odbcinst` | never call `odbcinst`; edit the INI file |
| `--template FILE` | another odbcinst.ini template |

Without a path, the script looks for `libtrinoodbc.so` in `/usr/lib`, `/usr/lib64`, `/usr/local/lib`,
`/usr/local/lib64`, `/usr/lib/<multiarch>` and a few other directories.

Or by hand (the `[ODBC Drivers]` block of the template is for iODBC and is dropped here):

```sh
sed -e 's|@TRINO_ODBC_DRIVER_NAME@|Trino ODBC Driver|' \
    -e 's|@TRINO_ODBC_DRIVER_PATH@|/usr/local/lib/libtrinoodbc.so|' \
    -e 's|@TRINO_ODBC_VERSION@|1.0.0|' \
    -e '/^;/d' -e '/^\[ODBC Drivers\]/,/^$/d' src/platform/unix/odbcinst.ini.in > /tmp/trino-driver.ini
sudo odbcinst -i -d -f /tmp/trino-driver.ini
```

Either way, the result is this section in the `odbcinst.ini` that unixODBC reads:

```ini
[Trino ODBC Driver]
Description=Trino ODBC Driver 1.0.0 (ODBC 3.8, Unicode)
Driver=/usr/local/lib/libtrinoodbc.so
Setup=/usr/local/lib/libtrinoodbc.so
APILevel=2
ConnectFunctions=YYN
DriverODBCVer=03.80
FileUsage=0
SQLLevel=1
UsageCount=1
```

`odbcinst -j` prints which `odbcinst.ini` / `odbc.ini` files unixODBC reads.
`odbcinst -q -d -n "Trino ODBC Driver"` shows the registration.

## Creating a DSN

System DSNs live in `/etc/odbc.ini`, user DSNs in `~/.odbc.ini`:

```ini
[TrinoProd]
Driver=Trino ODBC Driver
Description=Production Trino (LDAP)
Host=trino.corp.example.com
Port=8443
AuthenticationType=LDAP
UID=jdoe
SSL=1
TrustedCerts=/etc/ssl/certs/corp-root-ca.pem
Catalog=hive
Schema=default
TimeZone=Europe/Istanbul
```

**Time zone.** Without a `TimeZone` key the session runs in `Europe/Istanbul` (UTC+03:00, the
project default) and `timestamp with time zone` values are returned as Istanbul wall-clock time.
Change it per DSN (or in the connection string) with `TimeZone=<IANA name>` (for example
`TimeZone=Europe/Berlin`) or a fixed offset (`TimeZone=+05:30`), `TimeZone=system` for the zone of
the machine the driver runs on, and `TimestampWithTimeZoneMode=utc` for zoned timestamps in UTC
([CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md#default-time-zone-europeistanbul)).

`src/platform/unix/odbc.ini.example` has one section per authentication type (none, LDAP, JWT,
Kerberos, OAuth2, client certificate). Every key of [CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md)
is valid. Values containing `;` are written in braces, for example `SessionProperties={a=1;b=2}`.

TLS defaults: without an explicit `SSL=` key, TLS is enabled when the port is 443 or when any
authentication is configured (a `PWD`, a token, or an `AuthenticationType` other than None).
`SSL=0` or `AllowInsecurePassword=1` turns the automatic TLS off. `AllowInsecurePassword=1` also
allows LDAP, JWT, Kerberos and OAuth2 over plain HTTP (development only). Certificate
authentication always requires TLS. Without a `Port`, the default is 443 with TLS and 8080 without.

## Verifying

```sh
isql -v TrinoProd jdoe secret
SQL> SELECT version(), current_user;
```

Without a DSN:

```sh
isql -v -k "Driver=Trino ODBC Driver;Host=trino.corp.example.com;Port=8443;UID=jdoe;PWD=secret"
```

`iusql` exercises the wide-character (`W`) entry points. Python:

```sh
pip install pyodbc
python3 -c "import pyodbc; print(pyodbc.connect('DSN=TrinoProd;PWD=secret').execute('SELECT 1').fetchone())"
```

When `LogLevel` / `TRINO_ODBC_LOG_LEVEL` is set, the driver log goes to
`$TMPDIR/trino_odbc_<pid>.log` (or `/tmp/trino_odbc_<pid>.log` without `TMPDIR`). `LogPath` /
`TRINO_ODBC_LOG_PATH` name a file or a directory instead.

## Benchmarking

`trino_odbc_bench` (`tools/trino_odbc_bench.cpp`) measures fetch throughput. It `dlopen()`s the
driver library itself, so no driver manager or DSN is involved. It is part of the default build
(`cmake --build build` builds it). To build only the tool and the driver it depends on:

```sh
cmake --build build --target trino_odbc_bench          # -> build/trino_odbc_bench
```

Usage:

```
trino_odbc_bench "<connection string with Driver=<path to libtrinoodbc>>" [--sql "<query>"]
                 [--array-size N] [--narrow] [--getdata]
```

| Argument | Default | Meaning |
|---|---|---|
| connection string | required | Must contain `Driver=` (or `DRIVER=`) followed by the **path** of the driver library, unbraced and before the next `;`. The whole string is passed to `SQLDriverConnect` unchanged. |
| `--sql` | `SELECT * FROM tpch.sf1.orders` | the query |
| `--array-size N` | `1000` | rows per `SQLFetch` (`SQL_ATTR_ROW_ARRAY_SIZE`) |
| `--narrow` | off | bind character columns as `SQL_C_CHAR` instead of `SQL_C_WCHAR` |
| `--getdata` | off | fetch row by row with `SQLGetData` instead of bound block cursors |

Integer types are bound as `SQL_C_SBIGINT`, floating point as `SQL_C_DOUBLE`, `boolean` as
`SQL_C_BIT`, `date` / `timestamp` as the ODBC structs and everything else (including `decimal`) as
text of at most 4000 characters. Text is bound as `SQL_C_WCHAR` by default, the way Power BI reads
it. The tool prints one line per column, then for example:

```
rows=1500000 columns=9 payload=... MB warnings=0
connect ... ms | execute (first page) ... ms | fetch ... ms | ... rows/s | ... MB/s
```

The exit status is 0 on success, 1 when connect, execute or fetch fails (diagnostics are printed)
and 2 for usage errors or a driver that cannot be loaded.

```sh
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.so;Host=trino.corp.example.com;UID=jdoe;PWD=secret"
build/trino_odbc_bench "Driver=$PWD/build/libtrinoodbc.so;Host=localhost;Port=18080;UID=test" \
    --sql "SELECT * FROM tpch.tiny.orders" --getdata
```

For reference, `tpch.sf1.orders` (1.5M rows, 9 columns) was measured on an Apple M-series laptop
against Trino 483 in Docker. The classic JSON protocol over plain HTTP gave about 300k rows/s. The
spooled protocol over TLS gave 1.5-2.6M rows/s (0.6-1 s for 1.5M rows, with 40 segments downloaded
in parallel and acknowledged). See [PERFORMANCE.md](PERFORMANCE.md) for the tuning keys.

## Kerberos

`AuthenticationType=Kerberos` uses libcurl's SPNEGO support (GSSAPI/MIT Kerberos or Heimdal).
Check `curl --version` for `SPNEGO` and `Kerberos` under *Features*, obtain a ticket with `kinit`,
and make sure the coordinator's principal `HTTP/<host>@REALM` exists in your KDC. `KRB5_CONFIG`
and `KRB5CCNAME` are honoured by the Kerberos library as usual. Kerberos needs TLS unless
`AllowInsecurePassword=1` is set.

## Container builds and tests

* `tests/docker/run_linux_tests.sh` builds `tests/docker/Dockerfile.ubuntu` (Ubuntu 24.04,
  unixODBC, GCC). The build runs the unit tests, the mock-server integration tests and a
  driver-manager smoke test (`register_driver.sh`, `isql` with a DSN and with `-k`, `iusql`).
* `tests/docker/trino/run_trino.sh` starts a real Trino 483: plain HTTP without authentication on
  18080, and HTTPS with password authentication and the spooled protocol (segments in a MinIO
  container) on 18443, plus the same TLS coordinator over plain HTTP on 18081.
  `tests/docker/run_trino_tests.sh` runs the integration tests against them. Details are in
  [tests/docker/README.md](../tests/docker/README.md).

## Common problems

| Symptom | Fix |
|---|---|
| `[unixODBC][Driver Manager]Can't open lib '...': file not found` | Wrong `Driver=` path, or a missing dependency: `ldd /usr/local/lib/libtrinoodbc.so`. |
| `[unixODBC][Driver Manager]Data source name not found, and no default driver specified` | DSN not in the files `odbcinst -j` lists; check `ODBCINI`/`ODBCSYSINI`. |
| `odbcinst: command not found` (Debian/Ubuntu) | `sudo apt-get install odbcinst`; `register_driver.sh` falls back to editing `/etc/odbcinst.ini` without it. |
| Text arrives garbled in an application linked against iODBC | Build with `-DTRINO_ODBC_DRIVER_MANAGER=iodbc` for that application. |
| `28000 ... authentication requires SSL` | `SSL=0` with credentials. Remove it (TLS is then automatic), or add `AllowInsecurePassword=1` for a development cluster. |
| TLS errors with a distribution libcurl | Point `TrustedCerts` at your CA or install the CA into `/etc/ssl/certs` (`update-ca-certificates` / `update-ca-trust`). |

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for connection and query errors.
