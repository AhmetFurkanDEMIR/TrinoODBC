# Installing on Windows

> **Status.** The x64 package built by the mingw-w64 cross build
> (`TrinoODBC-<version>-windows-x64-setup.exe`, see
> [Building the Windows driver on macOS/Linux with mingw-w64](#building-the-windows-driver-on-macoslinux-with-mingw-w64))
> has been installed and used on Windows x64 by the maintainer: the Windows driver manager loads
> the driver, DSNs and connection strings connect, queries return data, and Power BI Desktop
> reads through it. The native MSVC + vcpkg route described next (`packaging/windows/build.ps1`,
> `installer.iss`, Inno Setup) uses the same sources, but it has **not yet been compiled with
> MSVC** in this project. Please report what differs if you use it.

## Requirements

* Windows 10 version 1903 (build 18362) or later, Windows 11, or Windows Server 2022 or later
  (Windows Server 2016 and 2019 are older builds). The installer refuses older systems
  (`MinVersion=10.0.18362`). The driver resolves IANA
  time zone names (`TimeZone=Europe/Berlin`, `timestamp with time zone` values, the Windows zone
  with `TimeZone=system`) through the system `icu.dll` (the combined ICU library Windows ships since 1903), which it loads from
  `System32` only. Without it (for example on a stripped-down image) `UTC`, fixed offsets such as
  `+03:00` and the zones of the driver's built-in Windows-to-IANA table (using the Windows registry
  rules, e.g. "Turkey Standard Time" for `Europe/Istanbul`) still resolve, and the default session
  zone `Europe/Istanbul` falls back to a compiled-in +03:00; other names are reported as unknown.
  The session zone is `Europe/Istanbul` unless `TimeZone` is set (project default). With
  `TimeZone=system` and a Windows zone that cannot be mapped to a usable IANA name, the driver
  sends no `X-Trino-Time-Zone` header (the server's time zone applies). Power BI Desktop, the on-premises data
  gateway and 64-bit Office need the **64-bit** driver (`build.ps1 -Arch x64`, the default).
  32-bit applications need a 32-bit build (`build.ps1 -Arch x86`, which configures CMake with
  `-A Win32` and the `x86-windows` vcpkg triplet).
* The Microsoft Visual C++ Redistributable for Visual Studio 2015-2022. The installer runs it only
  when `vc_redist.<arch>.exe` was put into the dist folder (`build.ps1 -DownloadVcRedist`);
  otherwise install it yourself.
* Network access from the client to the Trino coordinator port (8080 plain, 8443/443 TLS).

The driver is a single `trinoodbc.dll` plus the vcpkg DLLs it imports (`libcurl.dll`,
`zlib1.dll`, `zstd.dll`, `lz4.dll`, and `simdjson.dll` when vcpkg built it as a DLL). `build.ps1`
copies them next to the driver.

## Option A: the installer

The released installer, `TrinoODBC-<version>-windows-x64-setup.exe` on the
[Releases page](https://github.com/AhmetFurkanDEMIR/TrinoODBC/releases) (with a `SHA256SUMS` file),
is the NSIS package from the mingw-w64 cross build described in
[Building the Windows driver on macOS/Linux with mingw-w64](#building-the-windows-driver-on-macoslinux-with-mingw-w64)
and [packaging/windows/README-installer.md](../packaging/windows/README-installer.md). It is
self-contained (no Visual C++ redistributable), installs the driver, the documentation and the
Power BI connector, registers *Trino ODBC Driver* and adds an uninstaller. The rest of this
section describes the Inno Setup installer of the native MSVC build.

`packaging\windows\build.ps1 -Installer` builds the driver, assembles `dist\windows-<arch>\` and
compiles `packaging\windows\installer.iss` with Inno Setup 6 (`ISCC.exe` must be installed). The
result is `dist\TrinoODBC-<version>-<arch>.exe`. Run it as an administrator. It:

1. copies the driver, its dependency DLLs, `register_driver.ps1`, `VERSION.txt` and `docs\` to
   `C:\Program Files\TrinoODBC\` (`{autopf}`; a 32-bit installer uses `Program Files (x86)`),
2. registers *Trino ODBC Driver* under `HKLM\SOFTWARE\ODBC\ODBCINST.INI` (see the registry layout
   below) and in the *ODBC Drivers* list (a 32-bit installer writes the `WOW6432Node` view),
3. runs `vc_redist.<arch>.exe /install /quiet /norestart` when the redistributable was bundled,
4. adds an uninstaller (*Settings > Apps*) that removes the files and the registration again. It
   asks for confirmation first, because DSNs that use the driver stop working.

Existing DSNs are left untouched by an upgrade or an uninstall.

Both installers (this one and the NSIS installer of
[packaging/windows/README-installer.md](../packaging/windows/README-installer.md)) carry the Trino
logo: the setup `.exe` icon, the wizard images and the *Settings > Apps* icon
(`<install dir>\trinoodbc.ico`) come from `packaging/assets/generated/`, which
`packaging/assets/make_icons.sh` renders from `packaging/assets/trino-logo.svg`. `trinoodbc.dll`
has the same icon (resource `IDI_TRINOODBC`), and the DSN dialog shows it. Trino and the Trino logo
are trademarks of the Trino Software Foundation; this driver is an independent project and is not
affiliated with or endorsed by the Trino Software Foundation.

## Option B: register a built DLL yourself

For a development build (see below) run, in an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\register_driver.ps1 -DriverPath C:\path\to\dist\windows-x64\trinoodbc.dll
```

`register_driver.ps1` options: `-DriverPath` (default: `trinoodbc.dll` next to the script, then
`dist\windows-x64|x86|arm64\trinoodbc.dll`), `-Name` (default `Trino ODBC Driver`),
`-Description`, and `-Unregister`. It reads the DLL's PE header and registers a 32-bit DLL on
64-bit Windows under `WOW6432Node`. Running it twice is safe.

Or add the registry entries by hand (elevated `cmd.exe`):

```bat
set DRV=C:\Program Files\TrinoODBC\trinoodbc.dll
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers" /v "Trino ODBC Driver" /t REG_SZ /d Installed /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v Driver        /t REG_SZ    /d "%DRV%" /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v Setup         /t REG_SZ    /d "%DRV%" /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v Description   /t REG_SZ    /d "Trino ODBC Driver (ODBC 3.8, Unicode)" /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v APILevel      /t REG_SZ    /d 2 /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v ConnectFunctions /t REG_SZ /d YYN /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v DriverODBCVer /t REG_SZ    /d 03.80 /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v FileUsage     /t REG_SZ    /d 0 /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v SQLLevel      /t REG_SZ    /d 1 /f
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\Trino ODBC Driver" /v UsageCount    /t REG_DWORD /d 1 /f
```

A 32-bit driver on 64-bit Windows is registered under `HKLM\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI`
instead.

## Building from source

Requirements: Visual Studio 2022 with the *Desktop development with C++* workload (or the Build
Tools), CMake 3.16 or newer (3.20+ for `ctest --test-dir`), Git, and
[vcpkg](https://github.com/microsoft/vcpkg).

The scripted way:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"      # optional: without it build.ps1 clones vcpkg into <repo>\.vcpkg
powershell -ExecutionPolicy Bypass -File packaging\windows\build.ps1
```

`build.ps1` does the following:

1. It runs `vcpkg install curl[schannel] zstd lz4 simdjson` for the `<arch>-windows` triplet.
   `-SkipVcpkgInstall` skips this step.
2. It configures with the Visual Studio generator (`-A x64`, `Win32` or `ARM64`), the vcpkg
   toolchain, `-DTRINO_ODBC_DRIVER_MANAGER=windows`, `-DTRINO_ODBC_WITH_ZSTD=ON`,
   `-DTRINO_ODBC_WITH_LZ4=ON` and `-DTRINO_ODBC_USE_SYSTEM_SIMDJSON=ON`. Tests are off unless
   `-BuildTests` is given.
3. It builds the configuration (`-Config Release|RelWithDebInfo|Debug`, default `Release`).
4. It copies `trinoodbc.dll` (+ `.pdb`), the vcpkg DLLs the driver imports (found with `dumpbin
   /DEPENDENTS` when available), `register_driver.ps1`, `docs\*.md` and `VERSION.txt` into
   `dist\windows-<arch>\`.

Other parameters are `-Arch x64|x86|arm64`, `-VcpkgRoot`, `-Generator` (default
`"Visual Studio 17 2022"`), `-BuildDir` (default `build-win-<arch>`), `-Dist`, `-Clean`,
`-DownloadVcRedist` and `-Installer`. `Get-Help packaging\windows\build.ps1 -Detailed` lists
them.

By hand:

```powershell
vcpkg install curl[schannel] zstd lz4 simdjson --triplet x64-windows
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
      -DVCPKG_TARGET_TRIPLET=x64-windows -DTRINO_ODBC_DRIVER_MANAGER=windows
cmake --build build --config Release
ctest --test-dir build -C Release        # unit tests + integration tests against the Python mock server (needs python3 on PATH)
```

The driver is `build\Release\trinoodbc.dll`. The Windows driver manager (`odbc32.dll`) and the
installer library (`odbccp32.dll`) are part of Windows, so no ODBC SDK download is needed.
`TRINO_ODBC_DRIVER_MANAGER=auto` (the default) also selects `windows` on Windows.

Notes:

* Kerberos (`AuthenticationType=Kerberos`) needs a libcurl with SPNEGO support. On Windows that
  comes from the SSPI feature of vcpkg's curl port (`curl[schannel,sspi]`). To check, look for
  `SPNEGO` under *Features* in `curl.exe --version` from the same vcpkg tree.
* `-DTRINO_ODBC_WITH_ZSTD=OFF` / `-DTRINO_ODBC_WITH_LZ4=OFF` build without the compressed spooled
  encodings (`json` remains). With `auto` (the CMake default) a missing library silently disables
  the codec. `build.ps1` passes `ON`, so a missing package fails the configure step.
* A static build (`x64-windows-static` triplet) has not been tried. It also needs
  `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, because the CMake project does not switch the
  MSVC runtime to match the triplet.
* The benchmark utility `trino_odbc_bench` (see [Benchmarking](#benchmarking)) is part of the
  default build and has not been compiled with MSVC either. If it fails to build, build only the
  driver with `cmake --build build --config Release --target trinoodbc`.

## Building the Windows driver on macOS/Linux with mingw-w64

The x64 driver can also be cross-compiled without a Windows machine, with the GCC-based
mingw-w64 toolchain. The result is a single self-contained `trinoodbc.dll`: zstd, lz4, simdjson
and libcurl are linked in statically, and so are libgcc, libstdc++ and winpthread. The DLL imports
only DLLs that ship with Windows 10 1903 and later (`KERNEL32`, `ADVAPI32`, `USER32`, `SHELL32`,
`ODBCCP32`, `WS2_32`, `CRYPT32`, `bcrypt`, `Normaliz` and the Universal CRT `api-ms-win-crt-*`
forwarders). It needs neither the Visual C++ Redistributable nor any other DLL next to it.

> **Status.** On macOS (Apple silicon, Homebrew mingw-w64 14 / GCC 16.2, CMake 4.4) this build
> compiles and links without warnings, and the checks below pass. It uses the same sources, the
> same `.def` export list and the same `.rc` resources as the MSVC build. This is the build that
> ships in the x64 installer, and it has been verified on Windows x64: the installed driver is
> loaded by the Windows driver manager, DSNs and connection strings connect, queries run, and
> Power BI Desktop uses it. If you build it yourself, register it
> (`register_driver.ps1 -DriverPath ...`) and run a test connection and a query before you
> deploy it.

### Requirements

* macOS: `brew install mingw-w64 cmake ninja` (plus `curl` and `python3`, both included in macOS).
* Debian/Ubuntu: `apt install mingw-w64 cmake ninja-build curl python3`. Select the *posix*
  thread model with `update-alternatives --config x86_64-w64-mingw32-g++` (and the same for
  `-gcc`). The script also finds the `x86_64-w64-mingw32-g++-posix` names by itself. The *win32*
  variant is refused, because the driver uses `std::thread` and `std::mutex`.
* Internet access for the first run. Later runs work offline.

### Running the script

```sh
packaging/windows/cross_build_mingw.sh                       # everything, default locations
packaging/windows/cross_build_mingw.sh --prefix ~/mingw-deps # keep the dependency builds elsewhere
packaging/windows/cross_build_mingw.sh --verify-only         # only re-check build-mingw/trinoodbc.dll
```

The script runs with macOS `/bin/bash` 3.2, `bash`, `zsh` or `sh`, and it prints every command
before running it. It stops at the first error with a message that names the failing step.
Re-running it is safe, because finished steps are skipped. A full first run takes about one
minute on a 10-core Apple silicon Mac. The steps are:

1. **Dependencies.** It downloads the release tarballs of zstd 1.5.7, lz4 1.10.0, simdjson 3.13.0
   and curl 8.14.1 into `<work-dir>/downloads`. It checks that each file is a valid `.tar.gz` with
   the expected top-level directory, and it compares the SHA-256 of these default versions
   against pinned values. It then builds each library as a **static** Windows x64 library with
   `cmake/toolchain-mingw-w64.cmake` and installs it into the prefix. A dependency is skipped when
   `<prefix>/share/trino-odbc-mingw/<name>-<version>.stamp` and its library exist.
   `--rebuild-deps` forces a rebuild. libcurl is configured with HTTP only, **Schannel** for TLS
   (the Windows certificate store, so no CA bundle is needed), **SSPI** (SPNEGO/Kerberos with the
   logged-on Windows account, NTLM), Windows IDN, the threaded resolver, IPv6 and Unicode file
   names. It has no zlib, brotli, nghttp2, libssh2, libpsl, libidn2 or LDAP. The script fails if
   the configured libcurl lacks Schannel, SSPI or Negotiate, or if it has a CA file or path compiled in.
2. **Driver.** It configures and builds the driver with:
   ```sh
   cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
         -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_BUILD_TYPE=Release -DTRINO_ODBC_DRIVER_MANAGER=windows \
         -DTRINO_ODBC_BUILD_TESTS=OFF -DTRINO_ODBC_WITH_ZSTD=ON -DTRINO_ODBC_WITH_LZ4=ON \
         -DTRINO_ODBC_USE_SYSTEM_SIMDJSON=ON
   cmake --build build-mingw --target trinoodbc
   ```
   If the prefix differs from the one that is cached in `build-mingw`, the CMake cache is
   discarded first, so the build never keeps using libraries from an old prefix.
3. **Verification** (with `x86_64-w64-mingw32-objdump`, no Windows needed). The script fails
   unless the file is a PE32+ x86-64 DLL with ASLR (`DYNAMIC_BASE`) and DEP (`NX_COMPAT`), it
   imports Windows system DLLs only (for example, no `libstdc++-6.dll`, `libgcc_s_seh-1.dll`,
   `libwinpthread-1.dll`, `libcurl*.dll`, `zstd.dll`, `lz4.dll` or `simdjson.dll`), its export
   table is exactly `src/platform/win/trinoodbc.def` (111 `SQL*` functions plus `ConfigDSN`,
   `ConfigDSNW`, `ConfigDriver` and `ConfigDriverW`), and its `.rsrc` section holds the DSN dialog
   (including the time zone note), the Trino logo icon group (`IDI_TRINOODBC`, 16-256 px) and the
   `VERSIONINFO` of the project version.
4. **Staging** for the installer, unless `--no-stage` is given. The script writes a stripped copy
   of the DLL and verifies that copy again:
   ```
   dist/windows/stage/x64/trinoodbc.dll
   dist/windows/stage/docs/README.md, docs/*.md
   dist/windows/stage/VERSION                 (for example 1.0.0, no newline)
   dist/windows/stage/powerbi/TrinoODBC.mez   (from powerbi/build_mez.sh)
   dist/windows/stage/powerbi/README.md
   dist/windows/stage/assets/                 trinoodbc.ico, installer-welcome.bmp, installer-header.bmp,
                                              installer-small.bmp (from packaging/assets/generated)
   ```
   Only these paths are replaced. Other files in `dist/windows/stage` are left alone.

Options:

| Option | Default | Meaning |
|---|---|---|
| `--prefix DIR` | `build-mingw/prefix` | Install prefix of the static Windows dependencies |
| `--build-dir DIR` | `build-mingw` | CMake build directory of the driver |
| `--work-dir DIR` | `<build-dir>/deps` | Downloads, unpacked sources and dependency build trees |
| `--stage-dir DIR` | `dist/windows/stage` | Staging directory for the installer |
| `--triple NAME` | `x86_64-w64-mingw32` | mingw-w64 tool prefix |
| `--build-type T` | `Release` | CMake build type of the driver |
| `--jobs N` | number of CPUs | Parallel build jobs |
| `--rebuild-deps` | off | Rebuild the dependencies even when their stamps exist |
| `--no-stage` | off | Build and verify only |
| `--verify-only` | off | Only run the checks on `<build-dir>/trinoodbc.dll` |

`ZSTD_VERSION`, `LZ4_VERSION`, `SIMDJSON_VERSION` and `CURL_VERSION` in the environment select
other releases. For those, only the archive structure is checked, not a pinned SHA-256. Host
compiler variables (`CC`, `CXX`, `CFLAGS`, `LDFLAGS`, `PKG_CONFIG_PATH`, `CMAKE_PREFIX_PATH`, ...)
are ignored, with a note, so that Homebrew's settings cannot leak into the Windows build.

### The toolchain file

`cmake/toolchain-mingw-w64.cmake` can also be used by hand, as in step 2. It looks up
`x86_64-w64-mingw32-gcc`, `-g++` and `-windres` on `PATH` (the triple can be changed with
`-DTRINO_MINGW_TRIPLE=...`). It finds the mingw-w64 sysroot by asking the compiler where
`libkernel32.a` is, so it works with the Homebrew and the Linux layouts and has no hard-coded
paths. It restricts library, header and package lookups to that sysroot and to the directories
in `CMAKE_PREFIX_PATH`, so host libraries from `/opt/homebrew` or `/usr` cannot be linked by
mistake, and it points `pkg-config` at the prefixes only. By default it links with
`-static -static-libgcc -static-libstdc++` (`-DTRINO_MINGW_STATIC_RUNTIME=OFF` turns that off; the
DLL then needs the mingw runtime DLLs next to it).

For mingw-w64 the project's CMake files add the same `_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE
_UNICODE` definitions that the MSVC build uses, and they link `legacy_stdio_definitions` only
with MSVC.

### Differences from the MSVC build

* The compiler is GCC, not MSVC, and the CRT is the Universal CRT, which is part of Windows 10 and
  later. There is no `.pdb`: the unstripped `build-mingw/trinoodbc.dll` has DWARF debug
  information for gdb. The staged copy is stripped.
* Everything is linked statically, so there are no dependency DLLs to deploy (the vcpkg build
  ships `libcurl.dll`, `zstd.dll` and others). *Common problems* entries about missing
  dependency DLLs or the VC++ redistributable do not apply.
* libcurl speaks HTTP/1.1 only (no nghttp2), which is what the Trino client protocol uses. TLS
  and Kerberos use the same Windows components (Schannel, SSPI) as vcpkg's `curl[schannel,sspi]`.
  Client certificates therefore have the same Schannel limits in both builds.
* Only the `trinoodbc` target is built. `tools/trino_odbc_bench.cpp` does not compile for Windows
  yet (it includes `<sql.h>` before `<windows.h>`), and the unit tests would need Wine to run.
* simdjson picks its SIMD kernel (AVX2/AVX-512 or SSE4.2) at run time in both builds. simdjson
  supports MinGW-w64, and the GCC-compiled DLL has run on Windows x64; the project has not
  compared the individual kernels on different CPUs.

## Creating a DSN

1. Start the **64-bit** *ODBC Data Source Administrator*: `C:\Windows\System32\odbcad32.exe`
   (the one under `SysWOW64` is the 32-bit administrator and only shows 32-bit drivers).
2. *System DSN* (visible to services such as the Power BI gateway) or *User DSN* > *Add* >
   *Trino ODBC Driver*.
3. Fill in the dialog:

   | Field | Key it writes |
   |---|---|
   | Data source name, Description | `DSN`, `Description` |
   | Host, Port (empty = 8080, or 443 with SSL) | `Host`, `Port` |
   | Catalog, Schema | `Catalog`, `Schema` |
   | Authentication (None / LDAP / Kerberos / JWT / OAuth2 / Certificate) | `AuthenticationType` |
   | User, Password (*Save password* stores it in the DSN; otherwise the application must supply it) | `UID`, `PWD` |
   | Token (enabled for JWT) | `AccessToken` |
   | SSL, SSL verification (Full / CA / None), CA file | `SSL`, `SSLVerification`, `TrustedCerts` |
   | Client certificate, Client key, Client key password | `ClientCert`, `ClientKey`, `ClientKeyPassword` |
   | Session properties, Extra credentials, Roles | `SessionProperties`, `ExtraCredentials`, `Roles` |
   | Time zone (editable drop-down, pre-filled `Europe/Istanbul`), Source | `TimeZone`, `Source` |
   | Spooling, Target result size (1-128 MB) | `Spooling`, `TargetResultSize` |
   | Log level (0-5), Log path | `LogLevel`, `LogPath` |

   **Time zone.** The *Time zone* box is an editable drop-down. For a new DSN, and for a DSN
   without a `TimeZone` key, it is pre-filled with the driver default **`Europe/Istanbul`
   (UTC+3, Türkiye / Istanbul)**; the note next to it says so. Pick another zone from the list
   (`UTC`, `Europe/London`, `Europe/Berlin`, `Europe/Paris`, `Europe/Moscow`, `Asia/Dubai`,
   `Asia/Kolkata`, `Asia/Singapore`, `Asia/Tokyo`, `Australia/Sydney`, `America/New_York`,
   `America/Chicago`, `America/Denver`, `America/Los_Angeles`, `America/Sao_Paulo`, `system`) or
   type any other IANA name or an offset such as `+05:30`; `system` uses the Windows time zone.
   *OK* writes the value as `TimeZone` even when it is the default, so the DSN in the registry
   always shows the zone in use; editing an existing DSN shows its stored value. Clearing the box
   removes the key, and the driver then uses `Europe/Istanbul` again. The zone is sent to Trino as
   the session time zone (`X-Trino-Time-Zone`) and is used for `timestamp with time zone` values,
   see [CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md). The same box appears in the connection
   prompt of `SQLDriverConnect`.

   The dialog's title bar shows the Trino logo, and a line at the bottom states that the driver is
   not affiliated with the Trino Software Foundation.

   The dialog always writes `SSL=1` or `SSL=0`. That explicit value turns off the driver's
   automatic TLS for authenticated connections, so **tick SSL for every authentication type other
   than None**. If LDAP is selected without SSL, the dialog asks whether to store
   `AllowInsecurePassword=1` (development only). JWT, Kerberos and OAuth2 without SSL are refused
   at connect time unless `AllowInsecurePassword=1` is added to the DSN by hand. Certificate
   authentication always needs SSL.

   *Test connection* connects with the values entered through the driver manager (`odbc32.dll`),
   exactly like an application would. It needs the driver to be registered under a name.
4. *OK*. The DSN is stored under `HKLM\SOFTWARE\ODBC\ODBC.INI\<name>` (system) or
   `HKCU\SOFTWARE\ODBC\ODBC.INI\<name>` (user). Keys the dialog does not show (for example
   `RequestTimeout`, `MaxPrefetchBytes`, `DefaultStringColumnLength`, `AllowInsecurePassword`) can
   be added there with `regedit` or `Set-OdbcDsn`. They survive later edits in the dialog. Every
   key of [CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md) is valid in a DSN.

PowerShell alternative:

```powershell
Add-OdbcDsn -Name "TrinoProd" -DriverName "Trino ODBC Driver" -DsnType System -Platform 64-bit -SetPropertyValue @(
  "Host=trino.corp.example.com", "Port=8443", "AuthenticationType=LDAP", "UID=jdoe", "SSL=1",
  "Catalog=hive", "Schema=default", "TrustedCerts=C:\certs\corp-root-ca.pem", "TimeZone=Europe/Istanbul")
```

## Connecting without a DSN

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=8443;UID=jdoe;PWD=secret;Catalog=hive;Schema=default
```

A `PWD` without `AuthenticationType` selects LDAP. Because credentials are present and `SSL` is
not given, TLS is enabled automatically, and without a `Port` the default is then 443.
`SSL=0` or `AllowInsecurePassword=1` turns the automatic TLS off.

`SQLDriverConnect` with `SQL_DRIVER_PROMPT` always shows the connection dialog.
`SQL_DRIVER_COMPLETE` and `SQL_DRIVER_COMPLETE_REQUIRED` show it only when required values are
missing.

## Verifying

```powershell
Get-OdbcDriver -Name "Trino ODBC Driver" -Platform 64-bit
Get-OdbcDsn -DriverName "Trino ODBC Driver"

$c = New-Object System.Data.Odbc.OdbcConnection "DSN=TrinoProd;PWD=secret"
$c.Open()
(New-Object System.Data.Odbc.OdbcCommand("SELECT version(), current_user", $c)).ExecuteReader() | Format-Table
$c.Close()
```

When `LogLevel` (or the environment variable `TRINO_ODBC_LOG_LEVEL`) is greater than 0, the
driver writes its log to `%TEMP%\trino_odbc_<pid>.log`. `LogPath` / `TRINO_ODBC_LOG_PATH` name a
file or a folder instead. For services (the gateway), set the environment variables system-wide
so they reach the service process. A service's `%TEMP%` is the service account's temporary folder.

## Benchmarking

`trino_odbc_bench` (`tools/trino_odbc_bench.cpp`) loads the driver DLL directly, without the
driver manager. It runs one query with a block cursor and bound columns and prints the connect,
first-page and fetch times together with rows/s and MB/s. It is built with the project
(`build\Release\trino_odbc_bench.exe`, target `trino_odbc_bench`). The options are the same as on
Linux/macOS, see [INSTALL-LINUX.md](INSTALL-LINUX.md#benchmarking). `Driver=` must be the DLL
path, not the registered name:

```powershell
build\Release\trino_odbc_bench.exe "Driver=C:\src\trino-odbc\build\Release\trinoodbc.dll;Host=trino.corp.example.com;Port=8443;UID=jdoe;PWD=secret"
```

## Uninstalling

Use *Settings > Apps* (installer builds), or run `register_driver.ps1 -Unregister` (or delete the
registry keys shown above) and then delete the installation directory. DSNs that reference the
driver stop working until the driver is installed again. Delete them in the ODBC administrator if
they are no longer needed.

## Common problems

| Symptom | Cause / fix |
|---|---|
| `IM002 Data source name not found and no default driver specified` | Bitness mismatch (64-bit application, 32-bit DSN/driver or vice versa) or a user DSN of another account. Create a *System* DSN with the 64-bit administrator. |
| `Specified driver could not be loaded due to system error 126` | A dependency DLL is missing next to `trinoodbc.dll` (copy `dist\windows-<arch>\*.dll`) or the VC++ redistributable is not installed. `dumpbin /dependents trinoodbc.dll` lists them. |
| `Specified driver could not be loaded due to system error 193` | 32-bit/64-bit mix: the registered DLL has the other bitness. |
| `28000 ... authentication requires SSL` | The DSN has `SSL=0` (the dialog's SSL box unticked) with LDAP, JWT, Kerberos or OAuth2. Tick SSL, or add `AllowInsecurePassword=1` for a development cluster. |
| Dialog does not appear for `SQL_DRIVER_PROMPT` (`IM008`) | The dialog could not be created (for example no usable window handle). Use `SQL_DRIVER_NOPROMPT` with a complete connection string. |

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for connection and query errors.
