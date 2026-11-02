# Contributing

Thanks for your interest in the Trino ODBC driver. This page explains how to build the driver and
run its tests on macOS, Linux and Windows with the scripts in this repository. Design documents:
[ARCHITECTURE.md](ARCHITECTURE.md) (the full design),
[docs/ARCHITECTURE-OVERVIEW.md](docs/ARCHITECTURE-OVERVIEW.md) (a short tour) and the per-module
notes in [docs/design/](docs/design/).

Bug reports and feature requests go to the
[issue tracker](https://github.com/AhmetFurkanDEMIR/TrinoODBC/issues). For a bug, include the
driver version, platform and driver manager, the Trino version, and a driver log at `LogLevel=4`
after removing host names, user names and anything else you do not want to publish (the log never
contains passwords or tokens).

## Ground rules

* C++17; the code must compile with GCC, Apple clang and MSVC. Follow the coding rules in
  [docs/design/IMPLEMENTATION-NOTES.md](docs/design/IMPLEMENTATION-NOTES.md) and ARCHITECTURE.md §11.
* Every change comes with tests: unit tests in `tests/unit/` (no network), integration tests in
  `tests/integration/` (against the Python mock server and, where it matters, a real Trino).
* Keep the documentation in sync: a new connection option goes into `docs/CONNECTION-OPTIONS.md`
  and ARCHITECTURE.md §6, a changed `SQLGetInfo` answer into `docs/ODBC-CAPABILITIES.md`, a type
  mapping change into `docs/TYPES.md` and `docs/design/TYPE-MAPPING-RULES.md`. `README.md`
  (English) and `README.tr.md` (Turkish) have the same structure; change both together.
* Never commit credentials, certificates or generated test assets, and never put real host names,
  user names, e-mail addresses or absolute paths of your machine into code, tests or documentation:
  use `example.com` hosts, placeholder users such as `jdoe` and paths relative to the repository.
  Build trees (`build*/`), `dist/` and `tests/docker/trino/generated/` are git-ignored; keep them
  that way.

## Building and testing on macOS (iODBC)

```sh
brew install cmake ninja curl simdjson zstd lz4 libiodbc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRINO_ODBC_DRIVER_MANAGER=iodbc
cmake --build build
ctest --test-dir build --output-on-failure    # unit tests + integration tests against the mock server
```

The mock server needs `python3` (stdlib only). Useful direct invocations:

```sh
build/tests/trino_odbc_unit_tests
build/tests/trino_odbc_integration_tests --mock                  # starts tests/mock_trino/mock_trino.py itself
build/tests/trino_odbc_integration_tests --mock --filter fetch   # only tests whose name contains "fetch"
```

To build against unixODBC instead (Homebrew `unixodbc`), use `-DTRINO_ODBC_DRIVER_MANAGER=unixodbc`;
`SQLWCHAR` is 4 bytes with iODBC and 2 bytes with unixODBC, so one build serves one driver manager.
`packaging/macos/build_pkg.sh` builds, tests and packages the driver as a `.pkg`
([docs/INSTALL-MACOS.md](docs/INSTALL-MACOS.md)).

For a sanitizer build add `-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`
and use a separate build directory.

## Building and testing on Linux (unixODBC)

```sh
sudo apt-get install build-essential cmake ninja-build unixodbc-dev libcurl4-openssl-dev \
    libzstd-dev liblz4-dev libsimdjson-dev python3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Without Linux at hand, `tests/docker/run_linux_tests.sh` builds the driver in an Ubuntu 24.04
container and runs the unit tests, the mock integration tests and an `isql` smoke test through
unixODBC; a successful image build is a passed test run. `packaging/linux/build_packages.sh`
produces `.deb` / `.rpm` / `.tar.gz` packages ([docs/INSTALL-LINUX.md](docs/INSTALL-LINUX.md)).

## Testing against a real Trino (macOS or Linux, Docker)

```sh
tests/docker/run_trino_tests.sh            # starts the containers, runs every configuration
tests/docker/run_trino_tests.sh --only plain --filter query
tests/docker/trino/run_trino.sh remove     # remove the containers afterwards
```

`tests/docker/trino/run_trino.sh` starts a plain Trino 483 (`localhost:18080`), a TLS Trino with a
file password authenticator (`localhost:18443`, throw-away test user `alice` / `secret`) and a MinIO
container that stores spooled result segments. Certificates and passwords are generated locally
into the git-ignored `tests/docker/trino/generated/`. Details and troubleshooting:
[tests/docker/README.md](tests/docker/README.md).

Any other server works too:

```sh
TRINO_ODBC_TEST_CONNSTR='Host=trino.example.com;Port=8443;SSL=1;UID=...;PWD=...;Catalog=tpch;Schema=tiny' \
    build/tests/trino_odbc_integration_tests
```

## Building on Windows

Native build with MSVC and vcpkg (PowerShell, Visual Studio 2022 with the C++ workload, CMake):

```powershell
packaging\windows\build.ps1 -BuildTests          # vcpkg dependencies, Release x64 build, unit tests, dist folder
packaging\windows\build.ps1 -Installer           # also builds the Inno Setup installer
packaging\windows\register_driver.ps1            # register the built driver (administrator)
```

Cross build on macOS or Linux with mingw-w64 (no Windows machine needed):

```sh
packaging/windows/cross_build_mingw.sh     # static dependencies + trinoodbc.dll, checked with objdump
packaging/windows/build_installer.sh       # NSIS installer from the staged files (needs makensis)
```

The mingw-w64 cross build and its NSIS installer are the shipped Windows package; the maintainer
has installed and used it on Windows x64, including from Power BI Desktop. The MSVC/vcpkg route
(`build.ps1`, Inno Setup) has not been run yet, so reports and fixes for it are especially
welcome. See [docs/INSTALL-WINDOWS.md](docs/INSTALL-WINDOWS.md) and
[packaging/windows/README-installer.md](packaging/windows/README-installer.md).

## Pull requests

1. Build and run `ctest` on at least one platform; for protocol or TLS changes also run
   `tests/docker/run_trino_tests.sh`.
2. Keep the build warning-free (`-DTRINO_ODBC_WERROR=ON` turns warnings into errors).
3. Describe what changed and how you tested it. Driver logs (`LogLevel=4`) never contain passwords
   or tokens, but check any log or connection string you attach for host names and user names you
   do not want to publish.
4. Third-party code or data added to the driver must be listed in
   [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) with its license.
