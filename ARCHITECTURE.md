# Trino ODBC Driver — Architecture & Implementation Contract

This document is the design contract shared by every module; read it before changing code.
Public interfaces live in the headers listed in §3. If a header must change, change it minimally
and keep every other module compiling.

## 1. Goals

* A complete ODBC 3.8 driver (ANSI + Unicode entry points) for Trino, written from scratch in C++17.
* First-class Power BI Desktop / Power BI Gateway compatibility (query folding, metadata, block
  cursors, SQLGetData chunking, Unicode everywhere).
* Feature parity with the commercial Starburst/Simba driver: LDAP/AD (HTTP Basic over TLS), JWT,
  Kerberos (SPNEGO via libcurl), OAuth2 external browser flow, client certificates, full TLS control
  (on/off, Full/CA/None verification, custom CA, system trust store, min TLS version), proxies,
  session properties, roles, extra credentials, client tags, time zone, transactions.
* High throughput on large results: background prefetch, bounded memory, Trino spooled protocol
  (json, json+zstd, json+lz4) with parallel segment download, column-major typed pages, zero
  re-parsing, no per-cell allocations on the fetch path.
* Portable: Windows (MSVC, Driver Manager odbc32), Linux (unixODBC), macOS (iODBC by default,
  unixODBC optional). `SQLWCHAR` is 2 bytes on Windows/unixODBC and 4 bytes on iODBC — all
  wide-string code goes through `util/unicode.h` and never assumes a width.

## 2. Repository layout

```
CMakeLists.txt                 top-level build (options in §9)
cmake/                         Find modules / helpers
src/platform.h                 platform + ODBC header includes, export macros, SQLWCHAR width
src/util/                      logging, strings, unicode, base64, url, time helpers, thread utils
src/core/                      handles (env/dbc/stmt/desc), diagnostics, connection config
src/types/                     Trino type signature parser, ODBC type mapping, type-info table
src/client/                    libcurl HTTP layer, Trino REST protocol, auth (basic/jwt/kerberos/oauth2)
src/result/                    Page/Column storage, JSON decoder (simdjson), spool decoders, prefetcher, ResultSet
src/convert/                   value -> SQL_C_* conversions, parameter -> Trino literal rendering, temporal/tz, numeric
src/sql/                       ODBC escape-sequence translator, parameter marker scanner, SQL helpers
src/catalog/                   SQLTables/SQLColumns/... implementations, SQLGetTypeInfo table
src/odbc/                      the exported SQL* entry points (thin: validate, lock, dispatch, map errors)
src/platform/win/              DllMain, .def exports, ConfigDSN/ConfigDriver + setup dialog (.rc)
src/platform/unix/             odbcinst templates, install helpers
tests/unit/                    self-contained C++ unit tests (no external framework)
tests/integration/             ODBC API tests: dlopen driver directly and through the Driver Manager
tests/mock_trino/              Python mock Trino server (protocol-accurate, controllable failure modes)
tests/docker/                  Dockerfiles for Linux (unixODBC) build+test and real Trino configs
tools/                         trino_odbc_bench (throughput benchmark that loads the driver without a driver manager)
docs/                          user documentation (install per OS, Power BI, LDAP/AD, SSL, options, perf, types)
docs/design/                   contributor design notes per module (coding rules, client, ODBC API, types, tests)
packaging/                     Windows (Inno/WiX script), macOS (pkg script), Linux (CPack deb/rpm)
powerbi/                       Power Query custom connector (.pq) wrapping the driver with Trino SQL capabilities
README.md, README.tr.md        project front page in English and Turkish (keep both in sync)
```

## 3. Cross-module headers (stable contracts)

| Header | Owns |
|---|---|
| `src/platform.h` | includes, `TRINO_ODBC_API`, `SQLWCHAR_SIZE`, `TRINO_ODBC_SQLWCHAR_UTF16/UTF32` |
| `src/util/log.h` | `Logger` (levels 0..5, file sink, thread-safe), `TRINO_LOG_*` macros |
| `src/util/unicode.h` | UTF-8 <-> SQLWCHAR (UTF-16/UTF-32) <-> system ANSI; length in chars/bytes |
| `src/util/strings.h` | trim, iequals, split, number formatting/parsing helpers, `OutBuffer` copy helpers |
| `src/util/errors.h` | `DriverError{sqlstate,message,native}`, `TrinoQueryError`, `HttpError`, `CancelledError` |
| `src/core/diag.h` | `DiagRecord`, `Diag` |
| `src/core/config.h` | `ConnectionConfig` + every DSN/connection-string key and enum |
| `src/core/handles.h` | `Environment`, `Connection`, `Statement`, `Descriptor`, `DescRecord`, handle registry |
| `src/types/trino_type.h` | `TrinoType` parse/format |
| `src/types/odbc_types.h` | `OdbcTypeInfo`, `mapTrinoType()`, `StorageKind`, type-info table for SQLGetTypeInfo |
| `src/client/trino_client.h` | `TrinoClient`, `QueryExecution` (a `RowSource`), `SessionState`, `ExecOptions`, `QueryStats` |
| `src/result/page.h` | `Column`, `Page`, `PageBuilder` |
| `src/result/row_source.h` | `RowSource`, `ResultColumn`, `MemoryRowSource` |
| `src/result/result_set.h` | `ResultSet` cursor |
| `src/convert/convert.h` | `convertToC()`, `GetDataState` |
| `src/convert/params.h` | `renderParameterLiteral()`, `ParamValue` |
| `src/sql/escape.h` | `translateOdbcEscapes()`, `countParameterMarkers()`, `splitOnParameterMarkers()` |
| `src/catalog/catalog.h` | catalog function entry points returning `MemoryRowSource` |

Everything not listed is module-private; a module may add private headers freely.

## 4. Handle model and threading

* Handles are heap objects starting with `uint32_t magic` (`ENV_MAGIC`=0x454E5600, `DBC_MAGIC`, `STMT_MAGIC`, `DESC_MAGIC`). `validateHandle<T>(SQLHANDLE)` checks magic **and** membership in a global registry (`HandleRegistry`, mutex-protected set) — a freed or garbage handle returns `SQL_INVALID_HANDLE`.
* Each Environment/Connection/Statement has a `std::recursive_mutex` locked for the duration of every API call on it (`HandleLock`). Descriptor operations lock their owning connection's mutex.
* `SQLCancel` never takes the statement mutex: it sets `Statement::cancelRequested` (atomic) and cancels the active `QueryExecution` (lock-free with respect to the statement: it aborts in-flight curl transfers via the progress callback and sends `DELETE nextUri` on a helper thread). It cancels a query that the server is still running even when no ODBC call is in progress (a Power BI cancel arrives between two `SQLFetch` calls); the next fetch then reports `HY008`. `IdleFetchTimeout` additionally cancels queries whose consumer stopped fetching.
* The prefetch thread of a `QueryExecution` owns its own curl easy handle(s). It never touches ODBC handles. Communication is through a bounded `PageQueue` (condition variables). Backpressure: the fetcher blocks when `queuedBytes >= config.maxPrefetchBytes` **or** `queuedPages >= config.prefetchPages`.
* All exceptions are caught at the API boundary (`src/odbc/*.cpp`) and converted to diagnostics. Internal code throws `DriverError` (SQLSTATE known), `TrinoQueryError` (server error JSON), `HttpError` (transport), `CancelledError`. Never let an exception escape an exported function.

## 5. Connection lifecycle

1. `SQLDriverConnect`/`SQLConnect`: parse connection string (`ConnectionConfig::parseConnectionString`), then for `DSN=` load every key of the DSN section via `SQLGetPrivateProfileString(section, NULL, ...)` enumeration (connection-string values win over DSN values). Validate (§6). `SQL_DRIVER_PROMPT`/`COMPLETE` on Windows shows the setup dialog (platform/win/setup); on Unix the driver returns `SQL_NO_DATA`-style behaviour of Simba: missing required fields -> error `28000`/`IM007`.
2. `TrinoClient::connect()` builds the curl share handle, resolves auth, and — unless `ValidateConnection=0` — runs `SELECT version()` to fill `Connection::serverVersion` and prove credentials. Result `X-Trino-Set-*` headers update `SessionState`.
3. Session state (`SessionState`) is the single source of truth for catalog, schema, path, session properties, roles, prepared statements (name -> sql), transaction id, authorization user. It is mutated only from response headers and from `SQLSetConnectAttr(SQL_ATTR_CURRENT_CATALOG)` (which runs `USE catalog` if the connection is live, otherwise stores it).
4. `SQLDisconnect`: cancels every open `QueryExecution`, closes statements, releases curl handles.

## 6. Configuration keys

All keys are case-insensitive. The first name is canonical; aliases are accepted in DSN and
connection strings (aliases exist so DSNs written for the Starburst/Simba driver or the Trino JDBC
URL keep working). Booleans accept `1/0/true/false/yes/no/on/off`.

| Key (aliases) | Type / default | Meaning |
|---|---|---|
| `DSN`, `Driver`, `Description` | string | standard |
| `Host` (`Server`, `HostName`) | string, required | Trino coordinator host |
| `Port` | int, 8080 (443 when SSL=1 and Port absent) | |
| `Catalog` (`Database`) | string | default catalog (`X-Trino-Catalog`) |
| `Schema` | string | default schema |
| `UID` (`User`, `Username`) | string | `X-Trino-User`; required unless Kerberos/OAuth2/Certificate resolves it |
| `PWD` (`Password`) | string | Basic auth password (LDAP/AD) |
| `AuthenticationType` (`AuthMech`, `Auth`) | enum: `None` (0), `LDAP` (1, alias `Password`, `Basic`), `Kerberos` (2), `JWT` (3, alias `Token`, `AccessToken`), `OAuth2` (4, alias `ExternalAuthentication`, `External`), `Certificate` (5) | default: `LDAP` if PWD given, `JWT` if AccessToken given, else `None` |
| `SSL` (`UseSSL`, `Encrypt`, `Secure`) | bool, default 0 (auto-1 if Port=443 or AuthenticationType != None) | https |
| `SSLVerification` (`SSLMode`) | enum `Full` / `CA` / `None`, default `Full` | Full = CA chain + hostname; CA = chain only; None = trust everything |
| `AllowSelfSignedServerCert` | bool | Simba alias: 1 -> `SSLVerification=None` |
| `AllowHostNameCNMismatch` | bool | Simba alias: 1 -> `SSLVerification=CA` (if Full) |
| `TrustedCerts` (`SSLCA`, `CAFile`, `SSLTrustStorePath`) | path | PEM bundle (curl CAINFO) |
| `CAPath` (`SSLCAPath`) | path | directory of hashed PEM certs |
| `UseSystemTrustStore` | bool, default 1 | on Windows uses `CURLSSLOPT_NATIVE_CA`; on macOS/Linux relies on curl's default bundle |
| `ClientCert` (`SSLCert`, `SSLKeyStorePath`) | path | PEM (or PKCS#12 when `.p12/.pfx`) client certificate for mTLS / Certificate auth |
| `ClientKey` (`SSLKey`) | path | PEM private key |
| `ClientKeyPassword` (`SSLKeyPassword`, `SSLKeyStorePassword`) | string | |
| `MinTLSVersion` | `1.2` (default) / `1.3` | `CURLOPT_SSLVERSION` |
| `AllowInsecurePassword` | bool, default 0 | permit Basic auth over plain http (Trino must also allow it) |
| `AccessToken` (`JWT`, `AuthToken`, `Token`) | string | Bearer token |
| `AccessTokenFile` | path | read token from file at connect (rotating tokens) |
| `KerberosServiceName` (`KerberosRemoteServiceName`, `KrbServiceName`) | string, default `HTTP` | SPNEGO service principal `<name>/<host>` |
| `KerberosDelegation` | bool | `CURLOPT_GSSAPI_DELEGATION` |
| `KerberosUseCanonicalHostname` | bool, default 1 | canonicalize host for SPN |
| `ExternalAuthTimeout` (`OAuth2Timeout`) | seconds, 120 | browser flow timeout |
| `ExternalAuthRedirectHandler` | `open` (default: launch browser) / `print` (log URL only) | |
| `Source` (`ApplicationName`) | string, default `trino-odbc` | `X-Trino-Source` |
| `ClientInfo` | string | `X-Trino-Client-Info` |
| `ClientTags` | comma list | `X-Trino-Client-Tags` |
| `TraceToken` | string | `X-Trino-Trace-Token` |
| `TimeZone` (`TimeZoneID`) | IANA name / offset / `system` (OS zone); default `Europe/Istanbul` (`kDefaultSessionTimeZone`, filled in by `finalize()`) | `X-Trino-Time-Zone`; target zone of the `session` modes |
| `Locale` (`Language`) | string | `X-Trino-Language` |
| `SessionProperties` | `k=v;k2=v2` (`;` or `,` separated; catalog.prop=v allowed) | `X-Trino-Session` |
| `ExtraCredentials` | `k=v;k2=v2` | `X-Trino-Extra-Credential` |
| `Roles` | `catalog=ROLE{name};system=ALL` or `catalog=name` | `X-Trino-Role` |
| `SessionUser` | string | `X-Trino-Original-User` / session authorization |
| `HttpProxy` (`Proxy`) | host:port | `CURLOPT_PROXY` http |
| `SocksProxy` | host:port | `CURLOPT_PROXY` socks5h |
| `ProxyUser`, `ProxyPassword` | string | |
| `ConnectTimeout` (`LoginTimeout`) | seconds, 30 | TCP/TLS connect |
| `RequestTimeout` (`Timeout`) | seconds, 60 | per HTTP request; 0 = none |
| `QueryTimeout` | seconds, 0 | default `SQL_ATTR_QUERY_TIMEOUT`; cancels query when exceeded |
| `MaxRetries` (`Retries`) | int, 5 | retries for 503/502/504/connection reset with exponential backoff (100ms..2s) |
| `Compression` (`EnableCompression`) | bool, default 1 | `Accept-Encoding: gzip` via curl |
| `Spooling` (`SpoolingEnabled`) | bool, default 1 | send `X-Trino-Query-Data-Encoding` |
| `QueryDataEncoding` | string, default `json+zstd,json+lz4,json` (filtered by compiled-in codecs) | |
| `TargetResultSize` | MB, 16 (max 128) | `targetResultSize` query param on nextUri |
| `PrefetchPages` | int, 4 | pages queued ahead |
| `MaxPrefetchBytes` (`PrefetchBufferMB`) | MB, 64 | queue memory cap |
| `MaxParallelSegments` | int, 4 | concurrent spooled segment downloads |
| `UseUnicodeSqlCharacterTypes` (`UnicodeTypes`) | bool, default 1 | report SQL_WCHAR/WVARCHAR/WLONGVARCHAR vs SQL_CHAR/VARCHAR/LONGVARCHAR |
| `DefaultStringColumnLength` (`MaxVarcharSize`, `StringColumnLength`) | int, 65535 | column size reported for unbounded varchar/json/complex |
| `UnboundedVarcharAsLongVarchar` | bool, default 0 | report unbounded varchar as (W)LONGVARCHAR instead of (W)VARCHAR |
| `TimestampWithTimeZoneMode` (`TimestampTZMode`) | `session` (default; `local`, `sessiontz`) / `utc` / `wallclock` / `string` | how `timestamp with time zone` is exposed; `session` = converted to the session time zone |
| `TimeWithTimeZoneMode` | `session` / `utc` / `wallclock` / `string` (default `string`) | |
| `IntervalMode` | `string` (default) / `interval` | expose intervals as text or SQL_INTERVAL_* |
| `UuidAsGuid` | bool, default 0 | SQL_GUID vs SQL_WVARCHAR(36) |
| `ComplexTypesAsJson` | bool, default 1 | array/map/row -> JSON text (always true today; reserved) |
| `DecimalAsDouble` | bool, default 0 | report decimal as SQL_DOUBLE (for tools without numeric support) |
| `ExplicitPrepare` | bool, default 0 | 0: `EXECUTE IMMEDIATE ... USING`; 1: header prepared statement + `EXECUTE name USING` |
| `MetadataCaseSensitive` (`AssumeLiteralNamesInMetadataCalls`) | bool, default 0 | treat catalog-function patterns as literals |
| `EnableTransactions` | bool, default 0 | report `SQL_TC_DML`, honour `SQL_ATTR_AUTOCOMMIT=0` |
| `ValidateConnection` | bool, default 1 | run `SELECT version()` at connect |
| `LogLevel` | 0..5 (0 off,1 error,2 warn,3 info,4 debug,5 trace) | env `TRINO_ODBC_LOG_LEVEL` overrides |
| `LogPath` | dir or file | env `TRINO_ODBC_LOG_PATH` overrides; default `%TEMP%`/`/tmp` `trino_odbc_<pid>.log` |
| `ExtraHeaders` | `Name:value;Name2:value2` | added verbatim to every request |
| `AnsiEncoding` | `utf8` (default on Unix) / `system` (default on Windows) | encoding of SQL_C_CHAR data and narrow API strings |
| `ReadOnly` | bool, default 0 | default `SQL_ATTR_ACCESS_MODE`; rejects DML client-side |

Validation rules: `Host` required; `LDAP` requires `UID`+`PWD` and (`SSL=1` or `AllowInsecurePassword=1`) else SQLSTATE `28000` "Password authentication requires SSL"; `JWT` requires token; `Certificate` requires `ClientCert`; `SSLVerification=None` logs a warning (level 2) once per connection.

Unknown keys are kept in `ConnectionConfig::unknown` and logged at level 2 (not an error).

## 7. Trino protocol (client/)

* `POST {scheme}://{host}:{port}/v1/statement` body = SQL (UTF-8). Headers: `X-Trino-User`, `X-Trino-Source`, `X-Trino-Catalog`, `X-Trino-Schema`, `X-Trino-Path`, `X-Trino-Time-Zone`, `X-Trino-Language`, `X-Trino-Session` (comma-separated `k=v`, values URL-encoded), `X-Trino-Role`, `X-Trino-Extra-Credential`, `X-Trino-Client-Info`, `X-Trino-Client-Tags`, `X-Trino-Trace-Token`, `X-Trino-Transaction-Id` (`NONE` when autocommit), `X-Trino-Prepared-Statement` (`name=<urlencoded sql>`, repeated), `X-Trino-Client-Capabilities: PARAMETRIC_DATETIME,SESSION_AUTHORIZATION`, `X-Trino-Query-Data-Encoding` (spooling), `X-Trino-Original-User` (session user), `User-Agent: TrinoODBC/<ver>`, `Authorization` (Basic/Bearer) or SPNEGO negotiated by curl.
* Response JSON fields used: `id`, `infoUri`, `nextUri`, `partialCancelUri`, `columns[] {name,type,typeSignature}`, `data`, `stats{state,queued,scheduled,completedSplits,totalSplits,processedRows,processedBytes,elapsedTimeMillis,progressPercentage}`, `error{message,errorCode,errorName,errorType,sqlState?,errorLocation{lineNumber,columnNumber},failureInfo{type,message}}`, `warnings[] {warningCode{code,name},message}`, `updateType`, `updateCount`.
* Response headers applied to `SessionState`: `X-Trino-Set-Catalog`, `X-Trino-Set-Schema`, `X-Trino-Set-Path`, `X-Trino-Set-Session` (`k=v`, value URL-decoded), `X-Trino-Clear-Session`, `X-Trino-Set-Role`, `X-Trino-Added-Prepare`, `X-Trino-Deallocated-Prepare`, `X-Trino-Started-Transaction-Id`, `X-Trino-Clear-Transaction-Id`, `X-Trino-Set-Authorization-User`, `X-Trino-Reset-Authorization-User`.
* Loop: while `nextUri` present: `GET nextUri` (append `?targetResultSize=NNMB` only if allowed — Trino accepts `targetResultSize` as a query param on the nextUri; keep any existing query string). HTTP 503 -> retry same URI with backoff (not counted as failure until `MaxRetries` consecutive). Other non-200 -> `HttpError`. `error` object -> `TrinoQueryError` (finish loop; **do not** fetch further). Cancel: `DELETE nextUri` (best effort, 5 s timeout, one retry; sent at most once per query and only while the server is still producing rows).
* Spooled protocol: when the request carried `X-Trino-Query-Data-Encoding`, `data` may be an **object** `{encoding, metadata, segments[]}`; each segment is `{type:"inline", data:"<base64>", metadata:{segmentSize,rowOffset,rowsCount,uncompressedSize?}}` or `{type:"spooled", uri, ackUri, headers:{name:[values]}, metadata:{...}}`. Spooled segment bytes come from `GET uri` with the given headers (no Trino auth headers), then `DELETE ackUri` after successful decode (fire-and-forget, not before the data is decoded). `encoding` = `json` | `json+zstd` | `json+lz4`; decompress to `uncompressedSize` bytes; the payload is a JSON array of rows exactly like `data`. Segments are delivered to the page queue **in `rowOffset` order** even if downloaded in parallel. When `data` is a plain array (server without spooling or `Spooling=0`), the classic path is used. The `X-Trino-Query-Data-Encoding` header must only be sent when at least one of the listed codecs is compiled in.
* Values (JSON -> `StorageKind`): boolean -> Bool; tinyint/smallint/integer/bigint -> Int64; real/double -> Double (JSON strings `NaN`, `Infinity`, `-Infinity` parsed to the IEEE values); varbinary -> Binary (base64-decoded); every other type -> String holding the exact textual form Trino sent (decimal digits, `YYYY-MM-DD`, `HH:MM:SS.fff…`, `YYYY-MM-DD HH:MM:SS.fff…[ Zone|±HH:MM]`, `1-2` / `1 02:03:04.005` intervals, uuid, ipaddress, json text; array/map/row are stored as their raw JSON text via `raw_json()`). `null` -> null bit.
* `QueryExecution` runs the POST and follows `nextUri` on the calling thread until (a) `columns` is known **and** the first data page (or `nextUri==null`) arrived, or (b) the query finished/failed. Only then does it start the prefetch thread. This makes `SQLExecute` synchronous with respect to errors and metadata.
* Statistics from the last response are kept in `QueryStats` for diagnostics/logging.

## 8. ODBC semantics (odbc/, core/, result/, convert/)

* **State machine** (`Statement::state`): `S1_Allocated`, `S2_Prepared`, `S3_Executed_NoCursor` (DML), `S4_CursorOpen`, `S5_Fetched` (cursor positioned), `S8_NeedData` (data-at-exec), `S11_Executing` (used only while a call is in flight for cancel semantics). Every entry point checks the allowed states and returns `HY010`/`24000` exactly as the ODBC spec state tables prescribe.
* **Descriptors** are real: implicit ARD/APD/IRD/IPD per statement; explicit descriptors via `SQLAllocHandle(SQL_HANDLE_DESC)`; `SQLSetStmtAttr(SQL_ATTR_APP_ROW_DESC/APP_PARAM_DESC)` swaps the pointer; freeing an explicit descriptor reverts statements to their implicit ones; `SQLBindCol`/`SQLBindParameter` write ARD/APD records; IRD is rebuilt from `ResultColumn` list after prepare/execute; IPD from `DESCRIBE INPUT`.
* **Prepare** stores the escape-translated SQL and parameter count. Metadata before execute (`SQLNumResultCols`, `SQLDescribeCol`, `SQLColAttribute`, `SQLDescribeParam`, `SQLNumParams`) is obtained lazily: `DESCRIBE OUTPUT <name>` / `DESCRIBE INPUT <name>` with the SQL passed through `X-Trino-Prepared-Statement` (name = `odbc_stmt_<n>`); results cached on the statement until re-prepare.
* **Execute with parameters**: default `EXECUTE IMMEDIATE '<sql with '' escaping>' USING <lit1>, <lit2>` (Trino >= 431); `ExplicitPrepare=1` sends the prepared-statement header and `EXECUTE name USING ...`. Literal rendering is in `convert/params.h` and is driven by the bound `ParameterType` (SQL type) with the C type as the source; NULL -> `CAST(NULL AS <type>)`. Data-at-exec (`SQL_DATA_AT_EXEC`, `SQL_LEN_DATA_AT_EXEC(n)`) fully supported via `SQLParamData`/`SQLPutData`. `SQL_ATTR_PARAMSET_SIZE > 1` executes sequentially; if the statement returns a result set and paramset > 1 -> `HYC00` (`SQL_PAS_NO_SELECT`).
* **Escapes**: `{d}`, `{t}`, `{ts}`, `{interval}`, `{fn ...}` (mapping table in `sql/escape.cpp`), `{oj ...}`, `{escape 'c'}`, `{call ...}`, `{? = call}`; nested; skipped inside string literals, quoted identifiers, and comments. `SQL_ATTR_NOSCAN=SQL_NOSCAN_ON` disables. `SQLNativeSql` exposes the translator.
* **Cursor**: forward-only, read-only, block cursors via `SQL_ATTR_ROW_ARRAY_SIZE` up to any size, `SQL_ATTR_ROW_BIND_TYPE` (column-wise/row-wise), `SQL_ATTR_ROW_BIND_OFFSET_PTR`, `SQL_ATTR_ROW_STATUS_PTR`, `SQL_ATTR_ROWS_FETCHED_PTR`, `SQL_ATTR_ROW_OPERATION_PTR` honoured. `SQLFetchScroll` supports `SQL_FETCH_NEXT` only (others `HY106`). `SQLSetPos(SQL_POSITION)` selects the current row inside the rowset for `SQLGetData`. Requests for other cursor types are downgraded with `01S02`. `SQL_ATTR_MAX_ROWS` stops fetching (and cancels the query) after N rows.
* **SQLGetData** on any column, any order (`SQL_GD_ANY_COLUMN|SQL_GD_ANY_ORDER|SQL_GD_BLOCK|SQL_GD_BOUND`), chunked retrieval for char/wchar/binary with `01004` then `SQL_NO_DATA`; `GetDataState` tracks per-column byte offsets and is reset on every row move.
* **Conversions** (`convert/convert.cpp`) implement the full ODBC conversion matrix for our storage kinds: to `SQL_C_CHAR`, `SQL_C_WCHAR`, `SQL_C_BIT`, `SQL_C_(S|U)TINYINT`, `SQL_C_(S|U)SHORT`, `SQL_C_(S|U)LONG`, `SQL_C_(S|U)BIGINT`, `SQL_C_FLOAT`, `SQL_C_DOUBLE`, `SQL_C_NUMERIC`, `SQL_C_BINARY`, `SQL_C_TYPE_DATE/TIME/TIMESTAMP` (+ legacy `SQL_C_DATE/TIME/TIMESTAMP`), all `SQL_C_INTERVAL_*`, `SQL_C_GUID`, `SQL_C_DEFAULT`. SQLSTATEs: `01004` truncation, `01S07` fractional truncation, `22003` out of range, `22018` invalid character value, `22008` datetime overflow, `07006` restricted type. Numeric string->integer parsing accepts optional fraction only if it is zero (else `01S07`); double->integer rounds toward zero with `01S07` when fractional. Decimal <-> `SQL_NUMERIC_STRUCT` uses exact 128-bit arithmetic (`convert/numeric.cpp`).
* **Time zones** (`convert/temporal.cpp`): the session time zone defaults to `Europe/Istanbul` (`kDefaultSessionTimeZone` in `core/config.h`, a deliberate project default; `TimeZone=system` = OS zone) and is resolved once per session by `resolveSessionTimeZone()` for both `X-Trino-Time-Zone` and `ConvertOptions::sessionTimeZone`. `timestamp with time zone` values carry either an offset or an IANA zone name. `session` mode (default) converts the instant to the session zone (`timestampToZone`), `utc` mode to UTC; zone names resolve through `util/tz.h` (TZif parser over `/usr/share/zoneinfo`, `TZDIR`; on Windows through ICU `icu.dll` if present, else a compiled-in Windows-name -> IANA fallback table with registry rules; `Europe/Istanbul` finally falls back to a compiled-in +03:00). Unresolvable value zone -> value returned as wall-clock; unresolvable session zone -> value returned in UTC; either way one warning record `01000` per statement.
* **Diagnostics** message format: `[Trino][ODBC Driver] <text>` for driver errors, `[Trino][ODBC Driver][Trino] <server message>` for server errors with native code = Trino `errorCode`; server `errorName` -> SQLSTATE table in `core/diag.cpp` (`SYNTAX_ERROR`->`42000`, `TABLE_NOT_FOUND`->`42S02`, `COLUMN_NOT_FOUND`->`42S22`, `SCHEMA_NOT_FOUND`->`3F000`, `CATALOG_NOT_FOUND`->`3D000`, `PERMISSION_DENIED`->`42501`, `USER_CANCELED`->`HY008`, `EXCEEDED_TIME_LIMIT`/`EXCEEDED_*`->`HYT00`/`57014`, `DIVISION_BY_ZERO`->`22012`, `NUMERIC_VALUE_OUT_OF_RANGE`->`22003`, `INVALID_CAST_ARGUMENT`->`22018`, `INVALID_FUNCTION_ARGUMENT`->`22023`, `TYPE_MISMATCH`->`42804`, `AMBIGUOUS_NAME`->`42702`, `MISSING_*`/`INVALID_*` (USER_ERROR)->`42000`, `INTERNAL_ERROR`->`HY000`, `INSUFFICIENT_RESOURCES`->`HY001`, `EXTERNAL`->`HY000`, default `HY000`; Trino-supplied `sqlState` wins when present).
* **SQLGetInfo**: complete table in `odbc/api_info.cpp` (see docs/ODBC-CAPABILITIES.md generated from it). Values are dictated by the escape translator and Trino's grammar; keep them consistent (e.g. every function claimed in `SQL_STRING_FUNCTIONS` has a translation).
* **Catalog functions** run queries against `system.jdbc.*` (`catalogs`, `schemas`, `tables`, `columns`, `table_types`, `procedures`, `procedure_columns`) with `LIKE ... ESCAPE '\'` filters (or `=` when `SQL_ATTR_METADATA_ID` / `MetadataCaseSensitive` is on) and post-process the rows client-side into ODBC column layouts: `data_type`, `column_size`, `buffer_length`, `decimal_digits`, `num_prec_radix`, `sql_data_type`, `sql_datetime_sub`, `char_octet_length` are recomputed by `mapTrinoType(TrinoType::parse(type_name))` so `SQLColumns` and `SQLDescribeCol` always agree. `SQLPrimaryKeys`, `SQLForeignKeys`, `SQLStatistics`, `SQLSpecialColumns`, `SQLTablePrivileges`, `SQLColumnPrivileges` return empty result sets with the spec-mandated columns. `SQLGetTypeInfo` is a static table from `types/odbc_types.cpp` filtered by the requested type and by `UseUnicodeSqlCharacterTypes` etc. Special `SQLTables` enumerations (`SQL_ALL_CATALOGS`, `SQL_ALL_SCHEMAS`, `SQL_ALL_TABLE_TYPES`) are implemented.
* **Transactions**: `EnableTransactions=0` (default): `SQL_TXN_CAPABLE=SQL_TC_NONE`, `SQL_ATTR_AUTOCOMMIT=SQL_AUTOCOMMIT_OFF` -> `HYC00`. `EnableTransactions=1`: `SQL_TC_DML`; autocommit off makes the next statement send `X-Trino-Transaction-Id: NONE` and capture `X-Trino-Started-Transaction-Id`; `SQLEndTran` runs `COMMIT`/`ROLLBACK` with that id; `SQL_ATTR_TXN_ISOLATION` is not a session property: it is sent as part of `START TRANSACTION ISOLATION LEVEL ...` when the driver starts the transaction explicitly.
* **Unicode**: narrow entry points convert with `AnsiEncoding`; wide entry points use `SQLWCHAR` width from `platform.h`. All internal strings are UTF-8 `std::string`. Column data to `SQL_C_WCHAR` is transcoded from the UTF-8 arena directly into the application buffer (no intermediate std::u16string).

## 9. Build

* CMake >= 3.16, C++17. Targets: `trinoodbc` (shared library: `trinoodbc.dll`, `libtrinoodbc.so`, `libtrinoodbc.dylib`), `trinoodbc_core` (static, everything except exports — used by tests), `trino_odbc_unit_tests`, `trino_odbc_integration_tests`, `trinoodbc_setup` (Windows only, merged into the driver DLL as `ConfigDSN` exports).
* Options: `TRINO_ODBC_DRIVER_MANAGER=auto|unixodbc|iodbc|windows`, `TRINO_ODBC_WITH_ZSTD=auto`, `TRINO_ODBC_WITH_LZ4=auto`, `TRINO_ODBC_BUILD_TESTS=ON`, `TRINO_ODBC_USE_SYSTEM_SIMDJSON=auto` (else FetchContent v3.x), `TRINO_ODBC_INSTALL_ODBCINST=ON` (registers the driver with `odbcinst` on install for Unix).
* Dependencies: libcurl (required), simdjson (system or FetchContent), zstd/lz4 (optional, define `TRINO_ODBC_HAVE_ZSTD/LZ4`), ODBC headers + `odbcinst`/`iodbcinst`/`odbccp32` for `SQLGetPrivateProfileString`.
* Compiler flags: `-Wall -Wextra -Wpedantic -Werror=return-type` (GCC/Clang), `/W4 /permissive- /utf-8` (MSVC); no RTTI dependence, exceptions on; `-fvisibility=hidden` with explicit exports; Windows exports via `.def`.
* Version: `TRINO_ODBC_VERSION` = `1.0.0` -> `SQL_DRIVER_VER` `01.00.0000`.

## 10. Testing

* Unit tests (`tests/unit`): tiny framework in `tests/unit/test_framework.h` (`TEST_CASE`, `CHECK`, `CHECK_EQ`, `REQUIRE`), one file per module, target `trino_odbc_unit_tests` links `trinoodbc_core`.
* Mock Trino (`tests/mock_trino/mock_trino.py`, Python 3.9 stdlib only): implements `/v1/statement`, paging with `nextUri`, `columns` delayed to the second response, `stats`, errors, 503 storms, cancel via DELETE, Basic/Bearer auth checks, `X-Trino-Set-*` headers, `system.jdbc.*` catalog answers, `DESCRIBE INPUT/OUTPUT`, `EXECUTE IMMEDIATE` echo, `SELECT version()`, `USE`, spooled protocol with `json` inline+spooled segments (compressed codecs are covered by unit tests and the real Trino), all data types sample table `tpch.tiny.types_demo` synthesized in Python, TLS via `ssl` module with a self-signed cert generated by `tests/mock_trino/gen_cert.py`.
* Integration tests (`tests/integration`): C++ harness that `dlopen`s the driver directly (no DM required) with a `TRINO_ODBC_TEST_CONNSTR` env var; runs against mock (default) and against real Trino in Docker (`tests/docker/run_trino_tests.sh`). Also a Driver-Manager smoke test using `iodbctest`/`isql`.
* Linux: `tests/docker/Dockerfile.ubuntu` builds with unixODBC and runs unit+integration tests. Windows: `packaging/windows/build.ps1` (MSVC + vcpkg); `packaging/windows/cross_build_mingw.sh` cross-compiles the x64 DLL with mingw-w64 on macOS/Linux and checks its PE header, imports, exports and resources with `objdump`.

## 11. Coding rules

* C++17, no exceptions across the C API, no globals except the handle registry, logger and curl global init (`std::call_once`).
* Every exported function: `TRINO_ODBC_API SQLRETURN SQL_API SQLxxx(...)` in `src/odbc/*.cpp`; body = `return apiGuard(handle, [&](Statement& s){ ... })` pattern that validates the handle, locks, clears diag, catches exceptions, records `SQL_DIAG_RETURNCODE`, logs at trace level.
* Never `printf`/`std::cout`; use `TRINO_LOG_*`.
* No `using namespace std;` in headers. Namespace `trino_odbc` for everything internal.
* Comments explain *why*; every non-trivial function has a one-line doc comment. Keep files < ~1500 lines; split otherwise.
* Prefer `std::string_view` for inputs, `std::string` for owned text, `std::vector` over raw arrays, RAII for curl/zstd/lz4 handles.
