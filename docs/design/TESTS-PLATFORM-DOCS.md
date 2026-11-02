# Test infrastructure, platform glue, packaging and documentation (design notes)

## Mock Trino server (tests/mock_trino/mock_trino.py, Python 3.9 stdlib only)
`python3 mock_trino.py --port 18090 [--auth basic:user:pass | --auth bearer:TOKEN] [--tls cert.pem key.pem] [--page-rows 1000]
[--delay-ms 0] [--spooling]`. ThreadingHTTPServer. Protocol per ARCHITECTURE.md §7:
* `POST /v1/statement` body SQL -> JSON with id, infoUri, nextUri (`/v1/statement/queued/<id>/<token>/1`), stats.state QUEUED,
  NO columns on the first response (Power BI-relevant: drivers must loop). Second GET returns columns (+ data page), further
  GETs return more pages, last response has no nextUri and stats.state FINISHED. DELETE on any statement URI cancels (marks
  the query, later GETs return error USER_CANCELED). Each query needs the headers `X-Trino-User` (except when auth resolves it);
  echo `X-Trino-Set-Catalog`/`X-Trino-Set-Schema` on `USE a.b` / `USE a`; `X-Trino-Set-Session` on `SET SESSION k=v`;
  `X-Trino-Added-Prepare` when the request carried `X-Trino-Prepared-Statement`; `X-Trino-Started-Transaction-Id` when
  `X-Trino-Transaction-Id: NONE` and SQL is START TRANSACTION.
* Auth: basic -> 401 `WWW-Authenticate: Basic realm="Trino"` unless credentials match; bearer -> 401 unless token matches.
* SQL understood (case-insensitive, whitespace tolerant; unknown SQL -> error object `SYNTAX_ERROR` errorCode 1, errorType
  USER_ERROR, sqlState omitted, errorLocation {lineNumber:1,columnNumber:1}):
  - `SELECT version()` -> varchar "483-mock"; `SELECT 1` -> integer 1; `SELECT current_catalog`; `SELECT current_user`.
  - `SELECT * FROM types_demo` (or `... tpch.tiny.types_demo`) -> 3 rows covering: boolean, tinyint, smallint, integer, bigint,
    real (incl. NaN/Infinity strings), double, decimal(10,2), decimal(38,10), varchar, varchar(20), char(5), varbinary (base64),
    json, date, time(3), time(6) with time zone ("12:34:56.123456+05:30"), timestamp(3), timestamp(6) with time zone
    ("2024-03-10 02:30:00.123456 America/New_York" and "... +02:00"), interval year to month "1-2", interval day to second
    "3 04:05:06.789", array(integer), map(varchar,integer), row(a integer, b varchar), uuid, ipaddress, plus one row of NULLs
    and one row with unicode text ("Türkçe ğüşiöç 日本語 😀").
  - `SELECT * FROM big_table` optionally with `/* rows=N cols=K */` comment -> N rows (default 100000) of (bigint id,
    varchar name, double value, timestamp(3) ts, decimal(18,4) amount), paged by --page-rows; generated lazily per page.
  - `SELECT * FROM error_table` -> query fails on the 2nd response with errorName `DIVISION_BY_ZERO`, errorCode 65545,
    errorType USER_ERROR, message "Division by zero".
  - `SELECT * FROM slow_table` -> stays QUEUED for 10 GETs (each response nextUri, no columns) unless cancelled.
  - `SELECT * FROM flaky_table` -> first 3 GETs answer HTTP 503, then normal data.
  - `SELECT * FROM auth_table` -> 403 "Access Denied" HTTP status.
  - `SELECT * FROM warn_table` -> normal row plus `warnings:[{"warningCode":{"code":1,"name":"MOCK_WARNING"},"message":"mock warning"}]`.
  - `INSERT INTO t VALUES ...`/`UPDATE`/`DELETE` -> updateType "INSERT"/..., updateCount 3, no columns.
  - `CREATE TABLE ...`/`DROP TABLE ...` -> updateType "CREATE TABLE", no updateCount.
  - `DESCRIBE OUTPUT <name>` with `X-Trino-Prepared-Statement: <name>=<urlenc sql>` -> columns (Column Name varchar, Catalog
    varchar, Schema varchar, Table varchar, Type varchar, Type Size integer, Aliased boolean) derived from the prepared SQL
    using the same rules as executing it (types_demo/big_table/select 1/parametrized select); for INSERT -> zero rows.
  - `DESCRIBE INPUT <name>` -> (Position integer, Type varchar): one row per `?` in the prepared SQL with types from a
    simple heuristic (`WHERE id = ?` -> bigint, `name = ?` -> varchar, else `unknown`).
  - `EXECUTE IMMEDIATE '<sql>' USING <lits>` and `EXECUTE <name> USING <lits>` -> a single row with one varchar column
    `literals` containing the exact USING text as sent (so tests can assert rendering), plus a bigint column `count`.
    Inner `SELECT * FROM big_table WHERE id = ?` -> return the big_table row with that id.
  - `system.jdbc` queries: `SELECT ... FROM system.jdbc.catalogs` -> rows [tpch],[system],[mock]; `.schemas` -> (table_schem,
    table_catalog) rows for each catalog (tiny, sf1, information_schema / jdbc, runtime, information_schema / demo, information_schema);
    `.tables` -> (table_cat, table_schem, table_name, table_type, remarks, type_cat, type_schem, type_name,
    self_referencing_col_name, ref_generation) rows: tpch.tiny.{customer,orders,lineitem,nation,region} TABLE, mock.demo.{types_demo,
    big_table} TABLE, mock.demo.demo_view VIEW; `.columns` -> full JDBC column layout for those tables (customer: custkey bigint,
    name varchar(25), address varchar(40), nationkey bigint, phone varchar(15), acctbal double, mktsegment varchar(10), comment
    varchar(117); types_demo: all the types above; big_table: 5 columns); `.table_types` -> TABLE, VIEW; `.types` -> a few rows;
    `.procedures`/`.procedure_columns` -> empty. Apply `WHERE ... LIKE 'x' ESCAPE '\'` / `= 'x'` / `IN (...)` filters found in the
    SQL with a small regex-based evaluator supporting table_cat/table_schem/table_name/column_name/table_type predicates
    (translate LIKE to regex honouring `\` escapes). Also `SHOW CATALOGS`, `SHOW SCHEMAS FROM x`, `SHOW TABLES FROM x.y`.
  - `--spooling`: when the request has `X-Trino-Query-Data-Encoding` containing `json`, answer big_table/types_demo with
    the spooled protocol using encoding `json`: first segment inline (base64 of JSON rows), remaining segments `spooled`
    with uri `/v1/spooled/segments/<id>/<n>` (served by GET, require header `x-trino-mock-secret: s3cr3t` given in
    `headers`), ackUri `/v1/spooled/ack/<id>/<n>` (DELETE -> 200; record acks so tests can assert via
    `GET /mock/stats` JSON: {queries, cancelled, acks, requests}). Also serve `GET /mock/stats` and `POST /mock/reset`.
* Unknown paths -> 404. Log one line per request to stderr when `--verbose`.
`tests/mock_trino/gen_cert.py` creates a self-signed cert/key pair for localhost using `openssl` CLI (subprocess) with SAN
localhost/127.0.0.1 into a given directory (skips if present).

## Integration test harness (tests/integration/*.cpp, C++17, dlopen/LoadLibrary the driver, no driver manager)
`odbc_loader.h/.cpp`: loads `TRINO_ODBC_DRIVER_PATH` (env `TRINO_ODBC_DRIVER` overrides) and resolves every SQL* symbol into a
struct of function pointers (`Odbc` global) with typedefs matching the ODBC prototypes (wide variants too). Helper RAII for
handles, `diag(handleType, h)` returning vector of {sqlstate, native, message}, `check(rc, ...)` reporting failures via the
same test_framework.h macros (include "unit/test_framework.h"). `main.cpp`: parses `--mock` (starts the mock server as a
child process on a free port with `python3 TRINO_ODBC_MOCK_SERVER --port N --spooling`, waits until it answers, kills it at
exit) or `--connstr "<conn string>"` (real server) or env `TRINO_ODBC_TEST_CONNSTR`; `--filter name`. Connection string for
mock: `Driver=x;Host=127.0.0.1;Port=N;UID=test;Catalog=mock;Schema=demo;LogLevel=<env TRINO_ODBC_TEST_LOGLEVEL or 0>`.
Test files (each TEST_CASE names prefixed by file): test_connect (SQLDriverConnectW/A, out string, SQL_DRIVER_NOPROMPT,
wrong host -> 08001 + diag prefix; for the auth tests the harness starts extra mock instances on other ports, one with
`--auth basic:alice:secret` and one with `--auth bearer:<token>`: correct/incorrect password, Bearer token,
AllowInsecurePassword rule (LDAP over http without flag -> 28000 before any request)), test_metadata (SQLGetInfo string/
numeric for ~40 keys incl. buffer-too-small 01004, SQLGetFunctions, SQLGetTypeInfo rows ordered & DATA_TYPE values,
SQLGetEnvAttr/SQLGetConnectAttr/SQLGetStmtAttr round trips, descriptors: SQLGetStmtAttr(SQL_ATTR_APP_ROW_DESC) +
SQLSetDescField binding path), test_query (SELECT 1; types_demo every column via SQLGetData to SQL_C_CHAR/WCHAR and native
C types, NULL indicators, NaN, timestamps with tz in the default session mode (Europe/Istanbul) and in utc/wallclock/string mode, decimals to SQL_C_NUMERIC, varbinary to SQL_C_BINARY,
unicode round trip, SQLDescribeCol/SQLColAttribute values consistent with SQLColumns for types_demo, SQLPrepare +
SQLNumResultCols/SQLDescribeCol BEFORE execute, SQLRowCount for INSERT, DDL, SQLMoreResults SQL_NO_DATA, USE + current
catalog attr, warnings -> SQL_SUCCESS_WITH_INFO 01000), test_fetch (big_table 250k rows with SQL_ATTR_ROW_ARRAY_SIZE 1000
bound columns column-wise and row-wise binding + bind offset + status array + rows fetched ptr, checksum of ids; SQLGetData
chunked retrieval of a long varchar in 7-byte / 3-wchar pieces incl. SQL_NO_DATA end; SQL_ATTR_MAX_ROWS; SQLCloseCursor
mid-result then rerun; SQLCancel from another thread on slow_table -> HY008; flaky_table succeeds; error_table -> SQL_ERROR
with 22012 during fetch; spooled path stats show acks > 0; timing printed as info), test_params (SQLBindParameter with
SQL_C_CHAR/WCHAR/SLONG/SBIGINT/DOUBLE/BIT/TYPE_DATE/TYPE_TIMESTAMP/NUMERIC/BINARY/NULL and assert the mock's `literals`
column text: `'abc'`, `42`, `DOUBLE '1.5'`, `true`, `DATE '2024-01-02'`, `TIMESTAMP '2024-01-02 03:04:05.678'`,
`DECIMAL '12.34'`, `X'0102'`, `CAST(NULL AS varchar)`; data-at-exec with SQLParamData/SQLPutData in 2 chunks; SQLNumParams,
SQLDescribeParam; paramset size 2 for INSERT), test_catalog (SQLTables all forms incl. SQL_ALL_CATALOGS/SCHEMAS/TABLE_TYPES,
patterns with `_`/`%`/escapes, table type filter "TABLE,VIEW" and "'TABLE'", SQLColumns column layout (18 columns) & DATA_TYPE
consistency, SQLPrimaryKeys/ForeignKeys/Statistics/SpecialColumns/Procedures/TablePrivileges empty with correct column
counts, SQL_ATTR_METADATA_ID literal behaviour, W variants), test_errors (invalid handle, HY010 sequences, 24000, 07009,
HY090, HY003, HY092, SQLGetDiagField header fields, SQLGetDiagRec record ranges, SQL_NO_DATA on rec 2, 01004 on message
truncation, SQLError ODBC2), test_powerbi_flow (replays the exact call sequence Power BI Desktop performs: env 3.80,
SQLSetConnectAttrW(LOGIN_TIMEOUT), SQLDriverConnectW, SQLGetInfoW x30, SQLGetFunctions ALL, SQLGetTypeInfoW(SQL_ALL_TYPES),
SQLTablesW(SQL_ALL_CATALOGS), SQLTablesW(catalog,NULL...), SQLTablesW(cat, schema, "%", "TABLE,VIEW"), SQLColumnsW,
SQLPrimaryKeysW, SQLStatisticsW, SQLPrepareW("select ... limit 1000") + SQLNumResultCols + SQLDescribeColW/SQLColAttributeW
per column, SQLExecute, SQLBindCol with SQL_C_WCHAR/SQL_C_SBIGINT/SQL_C_DOUBLE/SQL_C_TYPE_TIMESTAMP, SQLSetStmtAttrW
ROW_ARRAY_SIZE/ROWS_FETCHED_PTR/ROW_STATUS_PTR, SQLFetchScroll loop, SQLGetData for long text, SQLFreeStmt(SQL_CLOSE),
SQLDisconnect). Print a summary line with rows/sec for the big fetch.
When run with `--connstr` against a real Trino, tests referencing mock-only tables are skipped (detect via
SQLGetInfo(SQL_DBMS_VER) containing "mock") and replaced by tpch.tiny equivalents where meaningful (customer 1500 rows,
`SELECT * FROM tpch.sf1.orders` 1.5M rows for the big fetch).

## Platform glue + packaging
* `src/platform/unix/entry.cpp`: `__attribute__((constructor))` calling Logger::configureFromEnvironment(); nothing else.
* `src/platform/unix/exports.map` (GNU version script `{ global: SQL*; ConfigDSN*; local: *; };`) and
  `src/platform/unix/exports.macos` (one `_SQLxxx` per line for every exported name in IMPLEMENTATION-NOTES.md).
* `src/platform/unix/odbcinst.ini.in` template (Driver / Setup / UsageCount / Description) and `odbc.ini.example`.
* Windows: `src/platform/win/dllmain.cpp` (DllMain: DisableThreadLibraryCalls, Logger env; no curl cleanup),
  `src/platform/win/trinoodbc.def` (all exports incl. ConfigDSN/ConfigDSNW/ConfigDriver/ConfigDriverW),
  `src/platform/win/setup.h/.cpp`: ConfigDSN (ODBC_ADD_DSN/CONFIG_DSN/REMOVE_DSN, parses attribute string, dialog with fields
  DSN, Description, Host, Port, Catalog, Schema, Authentication (combo: None/LDAP/Kerberos/JWT/OAuth2/Certificate), User,
  Password (stored in DSN only if the user ticks "Save password"), Token, SSL checkbox, SSL verification combo (Full/CA/None),
  CA file, Client cert/key, Advanced tab-less section: Session properties, Extra credentials, Roles, Time zone, Source,
  Log level, Log path, Spooling on/off, Target result size, "Test connection" button (loads the driver's own
  SQLDriverConnect through the DM: use SQLAllocHandle/SQLDriverConnectW from odbc32 with the assembled string) and
  `bool showConnectDialog(HWND, ConnectionConfig&)` used by SQLDriverConnect prompting; writes keys with
  SQLWritePrivateProfileString / SQLWriteDSNToIni / SQLRemoveDSNFromIni / SQLValidDSN; ConfigDriver handles
  ODBC_INSTALL_DRIVER/ODBC_REMOVE_DRIVER returning success. Dialog resource `trinoodbc.rc` + `resource.h` (DIALOGEX with
  the controls; use standard Win32 dialog procs; comctl32 not required). Also `trinoodbc.rc` VERSIONINFO.
* Packaging: `packaging/windows/build.ps1` (vcpkg install curl[schannel] zstd lz4 simdjson; cmake -A x64; builds Release;
  copies dll + deps to dist), `packaging/windows/installer.iss` (Inno Setup: installs dll to Program Files\TrinoODBC,
  registers the driver in HKLM\SOFTWARE\ODBC\ODBCINST.INI (Driver, Setup, APILevel 2, ConnectFunctions YYN, DriverODBCVer
  03.80, FileUsage 0, SQLLevel 1, UsageCount) and the "ODBC Drivers" list; uninstall removes), `packaging/windows/register_driver.ps1`
  (same registry work without an installer, for developers), `packaging/macos/build_pkg.sh` (builds one .pkg per driver
  manager with pkgbuild, installing to /Library/ODBC/TrinoODBC (iODBC) or /Library/ODBC/TrinoODBC-unixODBC, bundling the
  Homebrew dylib dependencies next to the driver, with a postinstall that registers the driver in /Library/ODBC/odbcinst.ini
  for iODBC or through Homebrew's `odbcinst` for unixODBC), `packaging/linux/register_driver.sh` (odbcinst -i -d -f template;
  fallback editing /etc/odbcinst.ini), `packaging/linux/build_packages.sh` (CPack DEB/RPM/TGZ),
  `packaging/windows/cross_build_mingw.sh` (mingw-w64 cross build of a self-contained x64 DLL on macOS/Linux, verified with
  objdump) and `packaging/windows/build_installer.sh` + `installer.nsi` (NSIS installer built on macOS/Linux).

## Docker test scripts
`tests/docker/Dockerfile.ubuntu`: ubuntu:24.04, apt installs build-essential cmake ninja-build unixodbc-dev libcurl4-openssl-dev
libzstd-dev liblz4-dev libsimdjson-dev python3, builds the project with -DTRINO_ODBC_DRIVER_MANAGER=unixodbc, runs unit +
integration (mock) tests, and a `isql` smoke test via odbcinst registration. `tests/docker/run_linux_tests.sh` builds/runs it.
`tests/docker/trino/` : `run_trino.sh` starting `trinodb/trino:483` (fallback trinodb/trino:478) on port 18080 plain, and a second
container `trino-odbc-tls` on 18443 with HTTPS + password file authenticator (config.properties, password-authenticator.properties
`password-authenticator.name=file`, `file.password-file=/etc/trino/password.db` with the throw-away test user `alice` /
password `secret`, PBKDF2 hash written by `gen_tls_assets.sh`), a self-signed certificate and PKCS12 keystore
(`/etc/trino/keystore.p12`) generated with openssl, and `spooling-manager.properties` (`spooling-manager.name=filesystem`
with an S3 location in a MinIO container, because Trino 483 cannot spool to a local directory;
`protocol.spooling.retrieval-mode=COORDINATOR_PROXY`, `fs.segment.encryption=false` for plain-HTTP MinIO). Generated assets go
to the git-ignored `tests/docker/trino/generated/`. `tests/docker/run_trino_tests.sh` runs the
integration tests against both (plain: `Host=localhost;Port=18080;UID=test;Catalog=tpch;Schema=tiny`, TLS: `...Port=18443;
SSL=1;SSLVerification=None;AuthenticationType=LDAP;UID=alice;PWD=secret` and again with `TrustedCerts=<ca.pem>` + Full).

## Documentation map
End-user documentation; README.md (English) and README.tr.md (Turkish, same structure, each linking to the other at the top):
what it is, features, platforms and status, repository layout, quick start per OS from the packages built by packaging/*,
Power BI setup in 5 steps, connection-string example, documentation table, license and third-party notices.
Contributor design notes (this file and the other module notes) live in docs/design/.
docs/INSTALL-WINDOWS.md, docs/INSTALL-MACOS.md, docs/INSTALL-LINUX.md (build from source + package install +
odbcinst/odbc.ini examples + verification with isql/iodbctest), docs/POWERBI.md (Desktop + Gateway: DSN vs connection string,
recommended DSN settings, query folding notes, custom connector installation from powerbi/, troubleshooting: "DataSource.Error",
timeouts, large models, DirectQuery vs Import, gateway service account & Kerberos), docs/CONNECTION-OPTIONS.md (every key from
ARCHITECTURE.md §6 with type/default/description/aliases, examples for each auth type), docs/LDAP-AD.md (Trino server
`password-authenticator.properties` for Active Directory — `ldap.url=ldaps://dc.corp.example.com:636`,
`ldap.user-bind-pattern=${USER}@corp.example.com` and `CN=${USER},OU=Users,DC=corp,DC=example,DC=com`, `ldap.user-base-dn`,
`ldap.group-auth-pattern`, `ldap.allow-insecure=false`, `ldap.ssl.truststore.path`; `http-server.authentication.type=PASSWORD`;
HTTPS setup; the driver side: AuthenticationType=LDAP, UID as sAMAccountName or UPN, PWD, SSL=1, TrustedCerts; common
errors), docs/SSL.md (modes Full/CA/None, corporate CA, Windows store (UseSystemTrustStore), client certs, MinTLSVersion,
self-signed dev setups, disabling), docs/PERFORMANCE.md (spooling, TargetResultSize, PrefetchPages, MaxPrefetchBytes,
MaxParallelSegments, ROW_ARRAY_SIZE guidance, Unicode vs ANSI, Power BI tips), docs/TYPES.md (mapping table),
docs/TROUBLESHOOTING.md (logging, SQLSTATE table, typical Power BI/Trino errors), docs/ODBC-CAPABILITIES.md (what SQLGetInfo
reports, supported escapes, cursor model, transactions), docs/ARCHITECTURE-OVERVIEW.md (short, for contributors).
powerbi/TrinoODBC.pq: Power Query custom connector (`Odbc.DataSource` with `SqlCapabilities` {LimitClauseKind =
LimitClauseKind.Limit, SupportsTop=false, Sql92Conformance=8, GroupByCapabilities=4, FractionalSecondsScale=3,
SupportsNumericLiterals..., StringLiteralEscapeCharacters = {"'"}}, `SQLGetInfo` overrides (SQL_SQL92_PREDICATES etc.),
`SQLGetTypeInfo` passthrough, `SQLColumns` fix-up for unbounded varchar, data source kind "TrinoODBC", `Authentication =
[UsernamePassword=[], Windows=[], Anonymous=[]]`, connection string built from Server/Port/Catalog options), plus
`powerbi/TrinoODBC.query.pq` test query, `powerbi/resources.resx`-free approach (strings in .pq), `powerbi/build_mez.sh`
(zip -> .mez) and `powerbi/README.md` (install into Documents\Power BI Desktop\Custom Connectors, security setting).
