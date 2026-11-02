# Third-party notices

The Trino ODBC driver is built on the open-source components listed below. This file says which
components end up in (or are linked by) each distributed artifact, which license covers them and
where their upstream sources and full license texts are.

The driver's own license is in `LICENSE` at the repository root.

> **Binary distributions.** Most of the licenses below (curl, BSD, MIT, zlib/libpng, Unicode)
> require their copyright notice and license text to accompany redistributed binaries. When you
> publish a package or installer, ship this file together with the full license texts of the
> components that the package actually contains (each upstream project's `COPYING` / `LICENSE`
> file, linked below).

## What each artifact contains

| Artifact | Built by | Contains / links |
|---|---|---|
| `libtrinoodbc.so` and the Linux `.deb` / `.rpm` / `.tar.gz` | `CMakeLists.txt`, `packaging/linux/build_packages.sh` | Links the distribution's shared libcurl, unixODBC (`libodbcinst`) or iODBC, zstd and lz4 dynamically; nothing is bundled. simdjson is linked from the system package or, when none is found, fetched with CMake `FetchContent` (tag `v3.13.0`) and linked statically. |
| macOS `.pkg` (`TrinoODBC-<version>-macos-<arch>.pkg`) | `packaging/macos/build_pkg.sh` | Links the system libcurl (`/usr/lib/libcurl.4.dylib`, part of macOS, not bundled). Copies the Homebrew dylibs it links next to the driver: simdjson, zstd, lz4 and `libiodbcinst` (iODBC package) or `libodbcinst` plus its `libltdl` dependency (unixODBC package). The installer pages show the Trino logo (see [Trademarks](#trademarks)). |
| macOS `.dmg` (`TrinoODBC-<version>-macos-<arch>.dmg`) | `packaging/macos/build_pkg.sh` (`hdiutil`) | The `.pkg` above, `README.md` and the documentation (including this file, `LICENSE` and `NOTICE`); the volume icon is the Trino logo. |
| `trinoodbc.dll` from the mingw-w64 cross build | `packaging/windows/cross_build_mingw.sh` | One self-contained DLL. Statically linked: libcurl 8.14.1 (Schannel + SSPI, no other third-party libraries), zstd 1.5.7, lz4 1.10.0, simdjson 3.13.0, the mingw-w64 runtime (CRT start-up code, winpthreads) and the GCC runtime (libgcc, libstdc++). It imports only Windows system DLLs. |
| NSIS installer (`TrinoODBC-<version>-windows-x64-setup.exe`) | `packaging/windows/build_installer.sh`, `installer.nsi` | The DLL above, the documentation and the Power BI connector, wrapped in the NSIS installer stub (LZMA compression). |
| MSVC build (`packaging/windows/build.ps1`) and Inno Setup installer (`installer.iss`) | vcpkg + MSVC, Inno Setup | `trinoodbc.dll` plus the vcpkg-built dependency DLLs copied next to it (libcurl, zlib, zstd, lz4 and, if built as a DLL, simdjson), optionally the Microsoft Visual C++ Redistributable (`-DownloadVcRedist`), wrapped in the Inno Setup installer stub. |
| `powerbi/TrinoODBC.mez` | `powerbi/build_mez.sh` | Only this project's Power Query code. |

## Components

### libcurl

* License: curl license (an MIT/X derivative), SPDX `curl`.
* Copyright: Daniel Stenberg and many contributors.
* Upstream: <https://curl.se/> — license text: <https://curl.se/docs/copyright.html>
* Used for: all HTTP(S) traffic. Statically linked into the mingw-w64 DLL; a DLL in the MSVC
  build; the system or distribution library on macOS and Linux.

### simdjson

* License: Apache License 2.0 **or** MIT (at the user's choice), SPDX `Apache-2.0 OR MIT`.
* Copyright: the simdjson authors.
* Upstream: <https://github.com/simdjson/simdjson> — `LICENSE` / `LICENSE-MIT` in the repository.
* Used for: JSON decoding of Trino responses. Statically linked into the mingw-w64 DLL and into
  FetchContent builds; a bundled dylib in the macOS package; a system library otherwise.

### Zstandard (zstd)

* License: BSD 3-Clause **or** GPL-2.0 (dual); this project uses it under the BSD 3-Clause license,
  SPDX `BSD-3-Clause`.
* Copyright: Meta Platforms, Inc. and affiliates.
* Upstream: <https://github.com/facebook/zstd> — `LICENSE` in the repository.
* Used for: the `json+zstd` spooled-segment encoding (optional at build time).

### LZ4

* License: BSD 2-Clause for the library (`lib/`), SPDX `BSD-2-Clause`.
* Copyright: Yann Collet.
* Upstream: <https://github.com/lz4/lz4> — `lib/LICENSE` in the repository.
* Used for: the `json+lz4` spooled-segment encoding (optional at build time).

### iODBC

* License: GNU Library General Public License 2 **or** BSD license (dual), SPDX
  `LGPL-2.0-only OR BSD-3-Clause`.
* Copyright: Ke Jin; OpenLink Software.
* Upstream: <https://www.iodbc.org/> and <https://github.com/openlink/iODBC> — `LICENSE`,
  `LICENSE.BSD`, `LICENSE.LGPL` in the repository.
* Used for: the ODBC headers of the macOS (iODBC) build and the `libiodbcinst` installer library
  (`SQLGetPrivateProfileString`), which the macOS iODBC package bundles as a separate dylib.

### unixODBC

* License: GNU Lesser General Public License 2.1 for the libraries, SPDX `LGPL-2.1-or-later`.
* Copyright: the unixODBC authors.
* Upstream: <https://www.unixodbc.org/> and <https://github.com/lurcher/unixODBC>
* Used for: the ODBC headers of the Linux build and the `libodbcinst` installer library, linked
  dynamically. The macOS unixODBC package bundles Homebrew's `libodbcinst` and GNU libtool's
  `libltdl` (LGPL-2.1-or-later, <https://www.gnu.org/software/libtool/>) as separate dylibs, which
  a user can replace; when distributing that package, the LGPL requires offering their source code.

### Windows ODBC headers and libraries

* The mingw-w64 cross build takes `sql.h`, `sqlext.h`, `odbcinst.h` and the `odbc32` / `odbccp32`
  import libraries from the mingw-w64 headers and CRT (see below). The MSVC build uses the Windows
  SDK. `odbc32.dll` and `odbccp32.dll` are part of Windows and are not redistributed.

### mingw-w64 runtime (cross build only)

* License: mingw-w64 CRT and headers: Zope Public License 2.1 and public-domain files;
  winpthreads: MIT and BSD 3-Clause. See `COPYING` and `COPYING.MinGW-w64-runtime` in the upstream
  sources.
* Upstream: <https://www.mingw-w64.org/> — <https://sourceforge.net/p/mingw-w64/mingw-w64/>
* Used for: start-up code, C runtime glue and `std::thread` support, statically linked into the
  cross-built `trinoodbc.dll`.

### GCC runtime libraries (cross build only)

* License: GPL-3.0-or-later **with the GCC Runtime Library Exception 3.1**, SPDX
  `GPL-3.0-or-later WITH GCC-exception-3.1`. The exception allows distributing the statically linked
  result under any license.
* Copyright: Free Software Foundation, Inc.
* Upstream: <https://gcc.gnu.org/> — exception text: <https://www.gnu.org/licenses/gcc-exception-3.1.html>
* Used for: libgcc and libstdc++, statically linked into the cross-built `trinoodbc.dll`.

### Unicode CLDR time-zone mapping data

* License: Unicode License v3, SPDX `Unicode-3.0`.
* Copyright: Unicode, Inc.
* Upstream: <https://github.com/unicode-org/cldr> (`common/supplemental/windowsZones.xml`) —
  license: <https://www.unicode.org/license.txt>
* Used for: the Windows-to-IANA time-zone table in `src/util/tz.cpp`, which is derived from
  `windowsZones.xml`.

### NSIS (Nullsoft Scriptable Install System)

* License: zlib/libpng license for NSIS and its installer stub; the LZMA decompression code used by
  `SetCompressor /SOLID lzma` is covered by the license terms listed on the NSIS license page.
* Copyright: Nullsoft and the NSIS contributors.
* Upstream: <https://nsis.sourceforge.io/> — license: <https://nsis.sourceforge.io/License>
* Used for: the Windows installer built by `packaging/windows/build_installer.sh`; the installer
  executable contains the NSIS stub and its standard plug-ins/UI (MUI2).

### Inno Setup (optional installer)

* License: Inno Setup License (a permissive license), see <https://jrsoftware.org/files/is/license.txt>
* Copyright: Jordan Russell and Martijn Laan.
* Upstream: <https://jrsoftware.org/isinfo.php>
* Used for: the alternative installer built from `packaging/windows/installer.iss`.

### vcpkg-built dependencies and Microsoft runtime (MSVC build only)

* zlib (pulled in by vcpkg's curl port): zlib license, SPDX `Zlib`, <https://zlib.net/>.
* Microsoft Visual C++ Redistributable (only when `build.ps1 -DownloadVcRedist` is used):
  distributed under the Microsoft Visual Studio license terms for redistributable code,
  <https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files>.
* vcpkg itself (MIT, <https://github.com/microsoft/vcpkg>) is a build tool and is not distributed.

## Build and test tools (not distributed)

These are used to build or test the driver but are not part of any artifact: CMake, Ninja,
Homebrew, the compilers (Apple clang, GCC, MSVC, mingw-w64 GCC), Python 3 (mock Trino server and
test helpers), OpenSSL command-line tools (test certificates), Docker, the `trinodb/trino` image
(Apache-2.0, <https://github.com/trinodb/trino>), the `minio/minio` and `minio/mc` images
(AGPL-3.0, <https://github.com/minio/minio>, pulled at test time only as a throw-away spooling
store) and the `ubuntu` base image of `tests/docker/Dockerfile.ubuntu`.

## Trademarks

"Trino" is a trademark of the Trino Software Foundation. This project is an independent ODBC
driver for Trino and is not affiliated with or endorsed by the Trino Software Foundation.

The Trino logo (the "Trino bunny", `packaging/assets/trino-logo.svg` as published on
<https://trino.io/>) is a trademark of the Trino Software Foundation. The icons and installer images
generated from it (`packaging/assets/generated/`, `packaging/macos/resources/`) appear in the
Windows installer and driver icon, the macOS installer package and the macOS disk image. The logo is
used only to indicate compatibility with Trino; no endorsement by the Trino Software Foundation is
implied, and the logo is not covered by this project's Apache License 2.0.
"Power BI", "Excel" and "Windows" are trademarks of Microsoft Corporation; "Starburst" and "Simba"
are trademarks of their respective owners and are mentioned only to describe compatibility.

## Logos used in the architecture diagram (docs/images)

* The OpenShift logotype (`docs/images/logos/openshift-logotype.svg`) is a trademark of Red Hat, Inc.
* The Kubernetes icon (`docs/images/logos/kubernetes-icon.svg`, CNCF artwork repository) is a trademark of The Linux Foundation.
* The Trino logo is a trademark of the Trino Software Foundation. The Windows and Power BI marks are drawn
  approximations; Windows and Power BI are trademarks of Microsoft Corporation.

They are used only to indicate the products the driver works with; no endorsement is implied.
