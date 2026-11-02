# Architecture overview (for contributors)

A short tour of the code base. The binding design contract with every detail is
[`../ARCHITECTURE.md`](../ARCHITECTURE.md); per-module design notes live in `docs/design/`. Read those
before changing a public header — every header under `src/` listed in ARCHITECTURE.md section 3
is a stable interface shared by several modules.

## Layout

```
src/platform.h        ODBC headers, export macro, SQLWCHAR width (2 bytes Windows/unixODBC, 4 bytes iODBC)
src/util/             logging (TRINO_LOG_*), strings, UTF-8 <-> SQLWCHAR/ANSI, base64/url, tz database, errors
src/core/             handles (Environment/Connection/Statement/Descriptor + registry), diagnostics, ConnectionConfig
src/types/            Trino type parser, Trino -> ODBC type mapping, SQLGetTypeInfo table
src/client/           libcurl transport, Trino REST protocol, auth (Basic/JWT/Kerberos/OAuth2), spooled segments, prefetch
src/result/           column-major Page/Column storage, simdjson decoder, RowSource, ResultSet cursor
src/convert/          value -> SQL_C_* conversions, parameter -> literal rendering, temporal/tz, exact decimal
src/sql/              ODBC escape translator, parameter-marker scanner, statement classifier
src/catalog/          SQLTables/SQLColumns/... over system.jdbc.*, SQLGetTypeInfo
src/odbc/             the exported SQL* entry points: validate, lock, dispatch, map errors (ANSI + W)
src/platform/win      DllMain, .def exports, ConfigDSN + setup dialog (mingw-w64 build verified on Windows x64)
src/platform/unix     constructor, export maps, odbcinst.ini template
tests/unit            self-contained C++ tests (tests/unit/test_framework.h)
tests/integration     dlopen the driver, run against the Python mock Trino or a real cluster
tests/mock_trino      protocol-accurate mock server with controllable failure modes
tools/                trino_odbc_bench (throughput benchmark that loads the driver without a driver manager)
packaging/            Windows (Inno Setup / PowerShell + vcpkg build.ps1), macOS (pkg), Linux (odbcinst / CPack)
powerbi/              Power Query custom connector (.pq -> .mez)
docs/                 user documentation
docs/design/          contributor design notes per module (read before changing a shared header)
```

## Data flow of a query

```
SQLExecDirect ──► odbc/api_statement.cpp: state check, escape translation (sql/), parameter literals (convert/params)
      │
      ▼
client/TrinoClient::execute ──► POST /v1/statement, follow nextUri until columns + first page
      │                          (synchronous: errors/metadata are reported by SQLExecute itself)
      ▼
QueryExecution (a RowSource) ──► prefetch thread keeps following nextUri; spooled segments are
      │                          downloaded + acknowledged by SpoolFetcher workers (client/spool_decoder)
      │                          and delivered in row order into a bounded page queue
      ▼
result/ResultSet ──► current Page (column-major, typed: Bool/Int64/Double/String/Binary, null bitmap)
      │
      ▼
SQLFetch / SQLGetData ──► convert/convertToC writes straight into the application buffers
```

Catalog functions build a `MemoryRowSource` from `system.jdbc.*` rows and reshape them into the
ODBC layouts; `ResultSet` does not care where pages come from.

## Key design points

* **Handles** are heap objects starting with a magic number and registered in a global
  `HandleRegistry`; `validateHandle<T>` rejects freed or garbage handles with
  `SQL_INVALID_HANDLE`. Every API call holds the handle's `recursive_mutex` (`HandleLock`).
* **Exceptions never cross the C ABI.** Internal code throws `DriverError` (known SQLSTATE),
  `TrinoQueryError` (server error object), `HttpError` (transport) or `CancelledError`; the
  `guard*` wrappers in `odbc/api_common.cpp` catch them in that order (there is no RTTI, so the
  most derived types are caught first), then `std::bad_alloc` (`HY001`) and `std::exception`
  (`HY000`), and turn them into diagnostic records.
* **SQLCancel** never takes the statement mutex: it sets an atomic flag and calls
  `QueryExecution::cancel()`, which aborts in-flight curl transfers from the progress callback and
  sends `DELETE nextUri` on a helper thread.
* **Prefetch** runs on a thread owned by the `QueryExecution`; after the synchronous phase the
  calling thread hands its `HttpClient` (curl easy handle) over to it, and it never touches ODBC
  handles. Spooled segments are fetched by up to `MaxParallelSegments` `SpoolFetcher` workers, each
  with its own curl handle; all handles of a connection share DNS cache, TLS sessions and the
  connection pool (`CurlShare`). Backpressure: the prefetch thread blocks when the queue holds
  `PrefetchPages` pages or `MaxPrefetchBytes` bytes. Internal queries (catalog functions,
  `SELECT version()`, `DESCRIBE`) run without a prefetch thread and without spooling.
* **Session state** (`SessionState`) is the single source of truth for catalog, schema, path,
  session properties, roles, prepared statements, transaction id and authorization user. It is
  seeded from the configuration at connect and afterwards changed only by response headers
  (`X-Trino-Set-*`, `X-Trino-Clear-Session`, `X-Trino-Added-Prepare` /
  `X-Trino-Deallocated-Prepare`, `X-Trino-Started-Transaction-Id` /
  `X-Trino-Clear-Transaction-Id`, `X-Trino-Reset-Authorization-User`) and by
  `SQL_ATTR_CURRENT_CATALOG`.
* **Auth and TLS** (`client/`): `finalize()` in `core/config.cpp` derives `SSL` (on for
  `Port=443` and for credentials unless `AllowInsecurePassword=1`), `validate()` and
  `TrinoClient::connect()` refuse credentials over plain HTTP without `AllowInsecurePassword=1`
  (Certificate never). Basic/Negotiate go through libcurl, bearer tokens (JWT, OAuth2) through an
  `Authorization` header; OAuth2 tokens are cached process-wide per `baseUrl|UID` only when `UID`
  is set. Every header name/value is checked for control characters before a request is sent
  (`HY000`).
* **Type mapping** happens in one place (`types/odbc_types.cpp`): `SQLDescribeCol`,
  `SQLColAttribute`, `SQLColumns` and `SQLGetTypeInfo` all derive from `mapTrinoType()`, so they
  cannot disagree.
* **Strings** are UTF-8 internally. Wide ODBC strings go through `util/unicode.h`, which handles
  both 2- and 4-byte `SQLWCHAR`; narrow strings use the configured `AnsiEncoding`.
* **Descriptors are real**: ARD/APD/IRD/IPD records drive binding, fetching and parameter
  rendering; `SQLBindCol`/`SQLBindParameter` are thin writers into them.
* **Configuration** (`core/config.cpp`) owns the key/alias table; `ARCHITECTURE.md` section 6 is
  its specification and [CONNECTION-OPTIONS.md](CONNECTION-OPTIONS.md) its user documentation.
  Keep the three in sync.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release          # -DTRINO_ODBC_DRIVER_MANAGER=auto|unixodbc|iodbc|windows
cmake --build build
ctest --test-dir build                                  # unit tests + integration tests against the mock server
./build/tests/trino_odbc_integration_tests --mock --filter fetch
TRINO_ODBC_TEST_CONNSTR='Host=localhost;Port=8080;UID=test;Catalog=tpch;Schema=tiny' \
    ./build/tests/trino_odbc_integration_tests          # against a real Trino
tests/docker/run_linux_tests.sh                         # Ubuntu + unixODBC container build and test
tests/docker/run_trino_tests.sh                         # real Trino 483 (plain + TLS/password/spooling via MinIO) in Docker
```

Verification status: the driver builds and passes 384 unit tests and the 149-test integration
suite on macOS (iODBC) and Ubuntu (unixODBC), against the mock server and against a real Trino 483
(plain HTTP, and HTTPS + password authentication with the spooled protocol, `json+zstd` / `json+lz4`
segments in MinIO; that setup needs `fs.segment.encryption=false` because MinIO is reached over
plain HTTP), plus an ASan/UBSan build on macOS. `packaging/windows/cross_build_mingw.sh`
cross-compiles and statically checks the x64 DLL with mingw-w64 on macOS/Linux, and the resulting
NSIS package has been installed and used on Windows x64 (Windows driver manager, DSN and
connection string, queries, Power BI Desktop). The released macOS `.pkg` and Linux `.deb` /
`.rpm` / `.tar.gz` packages have been installed and used the same way. The native MSVC build
(`packaging/windows/build.ps1` with vcpkg, Inno Setup) has **not** yet been compiled.

Options: `TRINO_ODBC_WITH_ZSTD` / `TRINO_ODBC_WITH_LZ4` (`auto|ON|OFF`), `TRINO_ODBC_USE_SYSTEM_SIMDJSON`
(`auto|ON|OFF`, otherwise FetchContent), `TRINO_ODBC_BUILD_TESTS`, `TRINO_ODBC_WERROR`,
`TRINO_ODBC_INSTALL_ODBCINST`. Dependencies: libcurl (required), simdjson, optional zstd/lz4, the
ODBC headers and `odbcinst` library of the chosen driver manager.

Targets: `trinoodbc` (the shared driver), `trinoodbc_core` (static library with everything but
the platform glue; linked by the tests), `trino_odbc_unit_tests`, `trino_odbc_integration_tests`,
`trino_odbc_bench` (see [PERFORMANCE.md](PERFORMANCE.md)).

A quick syntax-only check of a single file (add `-I` flags for your ODBC, simdjson and curl headers
if they are not on the default include path, e.g. `-I/opt/homebrew/include` with Homebrew):

```sh
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Isrc -Itests $(curl-config --cflags) src/sql/escape.cpp
```

## Coding rules

C++17, no RTTI dependence, exceptions on but never across the C API. Namespace `trino_odbc`.
No `printf`/`cout`: use `TRINO_LOG_*`. Portability first: GCC (Linux, and mingw-w64 for the verified
Windows build), Apple clang and MSVC (not yet exercised); POSIX calls only under `#ifdef TRINO_ODBC_UNIX`, Win32 under `#ifdef TRINO_ODBC_WINDOWS`; never assume the
`SQLWCHAR` width. `-Wall -Wextra -Wpedantic` clean. Comments explain *why*. Files stay under about
1 500 lines. Every exported function follows the `guard*` pattern and every error path yields a
diagnostic record, never a silent failure.

## Where to look

| I want to... | File |
|---|---|
| add a connection option | `src/core/config.h/.cpp` (key table, `applyKeyValue`, `toConnectionString`), then ARCHITECTURE.md section 6 and `docs/CONNECTION-OPTIONS.md`, `src/platform/win/setup.cpp` (dialog), `powerbi/TrinoODBC.pq` if Power BI should expose it |
| change how a Trino type is reported | `src/types/odbc_types.cpp` (+ `docs/design/TYPE-MAPPING-RULES.md`, `docs/TYPES.md`) |
| add a conversion | `src/convert/convert.cpp` (`convertToC`), tests in `tests/unit/test_convert.cpp` |
| add an `{fn}` translation | `src/sql/escape.cpp` and the bitmask in `src/odbc/api_info.cpp` (they must match) |
| change a `SQLGetInfo` answer | `src/odbc/api_info.cpp` and `docs/ODBC-CAPABILITIES.md` |
| map a new Trino error name | `src/core/diag.cpp` (`kErrorNames`) |
| touch the protocol | `src/client/trino_client.cpp`, `src/result/json_decoder.cpp`, `tests/mock_trino/mock_trino.py` |
