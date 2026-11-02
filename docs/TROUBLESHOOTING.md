# Troubleshooting

## 1. Turn on the driver log

| How | Value |
|---|---|
| DSN / connection string | `LogLevel=4;LogPath=C:\temp\trino` (a directory) or `LogPath=/tmp/trino-odbc.log` (a file) |
| Environment (overrides every DSN, useful for Power BI / gateway services) | `TRINO_ODBC_LOG_LEVEL=4` (a number 0..5), `TRINO_ODBC_LOG_PATH=C:\temp\trino` |

Levels: `0` off, `1` error, `2` warn, `3` info, `4` debug, `5` trace (every ODBC call with its
return code). The names `off`/`none`, `error`, `warn`/`warning`, `info`, `debug`, `trace` are
accepted in a DSN or connection string; numbers outside 0..5 are clamped.

Default file: `%TEMP%\trino_odbc_<pid>.log` on Windows, `$TMPDIR/trino_odbc_<pid>.log` (or
`/tmp/...`) on macOS/Linux. When `LogPath` names a directory the same file name is created inside
it. Lines look like `2026-09-27 12:00:00.123 [tid] DEBUG http: GET https://... -> 200`.

The logger is shared by every connection of the process: the level only ever rises to the highest
`LogLevel` any connection asked for (it is not lowered when that connection closes), and the first
non-empty path wins. `TRINO_ODBC_LOG_LEVEL` fixes the level for the whole process. Passwords,
tokens and extra credentials are never written (they appear as `***`). If the log file cannot be
opened, logging is silently disabled rather than failing the connection.

Driver-manager traces show the calls the application makes before the driver sees them:
unixODBC/iODBC `odbcinst.ini` `[ODBC] Trace=Yes TraceFile=/tmp/odbc.log`; Windows *ODBC Data
Source Administrator* > *Tracing* > *Start Tracing Now*.

## 2. Reading an error

Applications show the records returned by `SQLGetDiagRec`: a five-character SQLSTATE, a native
code and a message.

| Message prefix | Origin | Native code |
|---|---|---|
| `[Trino][ODBC Driver] ...` | the driver (configuration, conversion, ODBC state errors) | 0, or the HTTP status for transport errors |
| `[Trino][ODBC Driver][Trino] ...` | the Trino server (`error.message` of the query response) | Trino `errorCode` (e.g. 1 = SYNTAX_ERROR, 65545 = DIVISION_BY_ZERO) |

Power BI wraps them as `DataSource.Error: ODBC: ERROR [SQLSTATE] [Trino][ODBC Driver] ...`;
unixODBC `isql` prints `[SQLSTATE][unixODBC][Trino][ODBC Driver] ...`.

Warnings (SQLSTATE class `01`) come with `SQL_SUCCESS_WITH_INFO`, for example `01000` for a Trino
query warning, an unresolvable time zone or (from `SQLNativeSql`) an unknown `{fn}` passed
through unchanged, `01004` for
a truncated string, `01S00` for an unknown connection-string key that was ignored, `01S02` when an
unsupported attribute value was replaced by a supported one (also when `SQL_AUTOCOMMIT_OFF`
requested before connecting is reset because `EnableTransactions=0`).

## 3. SQLSTATE reference

### Driver-side

| SQLSTATE | Meaning | Typical cause |
|---|---|---|
| `08001` | Unable to connect | DNS, refused connection, TLS handshake failure, any transport error or non-2xx answer (other than 401/403) while `SELECT version()` runs at connect |
| `08004` | Server rejected the connection | Trino `INCOMPATIBLE_CLIENT` |
| `08002` | Connection already open | `SQLDriverConnect` twice on one handle |
| `08003` | Not connected | statement allocated on a disconnected handle |
| `08S01` | Communication link failure | connection dropped mid-query, HTTP 5xx after `MaxRetries`, non-2xx answer |
| `28000` | Authentication failed / invalid authorization | HTTP 401/403, missing `UID`/`PWD`/token, password over plain HTTP, `SSL` required |
| `HYT00` | Timeout | `RequestTimeout` per request, `QueryTimeout` / `SQL_ATTR_QUERY_TIMEOUT` for the query |
| `HYT01` | Connection timeout | `ConnectTimeout` / `SQL_ATTR_LOGIN_TIMEOUT` expired |
| `HY008` | Operation cancelled | `SQLCancel`, `SQLCancelHandle`, `IdleFetchTimeout`, or `USER_CANCELED` from the server |
| `HY000` | General error | configuration errors (`Host is required`, `Invalid value for <Key>: ...` for malformed or out-of-range option values such as `Port=70000`), `Invalid character in HTTP header` (control characters in a header value), spooled segment size/row-count mismatches, server `INTERNAL_ERROR` |
| `HY001` | Memory allocation | out of memory, server `INSUFFICIENT_RESOURCES` |
| `HY024` | Invalid attribute value | `HttpProxy` and `SocksProxy` both set; invalid `SQLSetConnectAttr` / `SQLSetStmtAttr` values |
| `HY010` | Function sequence error | e.g. `SQLExecute` before `SQLPrepare`, `SQLFetch` without result |
| `HY090` | Invalid string or buffer length | negative buffer length; a `SQL_C_WCHAR` buffer that cannot hold a single character of the value |
| `HY092` | Invalid attribute / option identifier | unknown `SQLSetConnectAttr` / `SQLSetStmtAttr` id |
| `HY096` | Information type out of range | unknown `SQLGetInfo` type (or a driver-manager-only type asked directly) |
| `HY106` | Fetch type out of range | `SQLFetchScroll` other than `SQL_FETCH_NEXT` |
| `HY109` | Invalid cursor position | `SQLSetPos` row number outside the rowset |
| `HYC00` | Optional feature not implemented | autocommit off without `EnableTransactions`, bookmarks, `SQLBulkOperations`, output parameters, `{? = call}`, paramset > 1 with a result set, Kerberos with a libcurl without SPNEGO, a spooled encoding the build does not include |
| `24000` | Invalid cursor state | `SQLGetData` before `SQLFetch`, `SQLExecute` while a cursor is open, `SQLCloseCursor` without cursor |
| `IM002` (driver) | Data source name not found | `DSN=` names a section that does not exist and the string has no `Driver=` |
| `25000` | Invalid transaction state | disconnect with an open driver transaction |
| `07002` | Wrong number of parameters | fewer parameters bound than `?` markers |
| `07006` | Restricted data type violation | conversion not allowed for the column/parameter |
| `07009` | Invalid descriptor index | column number 0 or above the column count |
| `01004` | String data right-truncated | buffer too small; continue with `SQLGetData` |
| `01S07` | Fractional truncation | fraction lost converting to an integer / `SQL_TIME_STRUCT` |
| `22003` | Numeric value out of range | value does not fit the C type / `SQL_NUMERIC_STRUCT` precision |
| `22008` | Datetime field overflow | date/time outside the C structure range |
| `22015` | Interval field overflow | interval does not fit `SQL_INTERVAL_STRUCT` |
| `22018` | Invalid character value for cast | text is not a number/date/uuid |
| `34000` | Invalid cursor name | empty or too long name in `SQLSetCursorName` |
| `42000` | Syntax error / read-only violation | `ReadOnly=1` with DML/DDL; malformed escape sequence |
| `IM002` | Data source name not found and no default driver specified | DSN missing or defined for the other bitness/driver manager |
| `IM007` | No data source or driver specified; dialog prohibited | `SQL_DRIVER_PROMPT` on Unix (no dialog) with missing values |
| `IM008` | Dialog failed | Windows setup dialog could not be created |

### Server errors (Trino `errorName` -> SQLSTATE)

A `sqlState` supplied by Trino wins; otherwise:

| Trino error | SQLSTATE |
|---|---|
| `SYNTAX_ERROR`, `MISSING_*`, `INVALID_*` (user errors), most other analysis errors | `42000` |
| `TABLE_NOT_FOUND`, `VIEW_NOT_FOUND`, `NOT_FOUND` | `42S02` |
| `COLUMN_NOT_FOUND` | `42S22` |
| `SCHEMA_NOT_FOUND` | `3F000` |
| `CATALOG_NOT_FOUND` | `3D000` |
| `TABLE_ALREADY_EXISTS`, `ALREADY_EXISTS` | `42S01` |
| `COLUMN_ALREADY_EXISTS`, `DUPLICATE_COLUMN_NAME` | `42S21` |
| `FUNCTION_NOT_FOUND`, `OPERATOR_NOT_FOUND`, `PROCEDURE_NOT_FOUND` | `42883` |
| `TYPE_NOT_FOUND` | `42704` |
| `TYPE_MISMATCH` | `42804` |
| `AMBIGUOUS_NAME` | `42702` |
| `PERMISSION_DENIED`, `SECURITY_ERROR` | `42501` |
| `USER_CANCELED`, `ABANDONED_QUERY`, `ADMINISTRATIVELY_KILLED`, `ADMINISTRATIVELY_PREEMPTED` | `HY008` |
| `EXCEEDED_TIME_LIMIT`, `PAGE_TRANSPORT_TIMEOUT` | `HYT00` |
| `AMBIGUOUS_FUNCTION_CALL`, `AMBIGUOUS_RETURN_TYPE` | `42725` |
| `INCOMPATIBLE_CLIENT` | `08004` |
| `EXCEEDED_*` (memory, cpu, spill, scan ...) | `57014` |
| `DIVISION_BY_ZERO` | `22012` |
| `NUMERIC_VALUE_OUT_OF_RANGE` | `22003` |
| `INVALID_CAST_ARGUMENT` | `22018` |
| `INVALID_FUNCTION_ARGUMENT` | `22023` |
| `NOT_SUPPORTED`, `UNSUPPORTED_*` | `0A000` |
| `CONSTRAINT_VIOLATION` | `23000` |
| `TRANSACTION_CONFLICT` | `40001` |
| `NOT_IN_TRANSACTION`, `UNKNOWN_TRANSACTION`, `TRANSACTION_ALREADY_ABORTED`, `AUTOCOMMIT_WRITE_CONFLICT` | `25000` |
| `READ_ONLY_VIOLATION` | `25006` |
| `SUBQUERY_MULTIPLE_ROWS` | `21000` |
| `INSUFFICIENT_RESOURCES`, `CLUSTER_OUT_OF_MEMORY`, `STACK_OVERFLOW` | `HY001` |
| `NO_NODES_AVAILABLE`, `SERVER_STARTING_UP`, `CATALOG_UNAVAILABLE` | `08001` |
| `TOO_MANY_REQUESTS_FAILED`, `PAGE_TRANSPORT_ERROR`, `REMOTE_HOST_GONE`, `SERVER_SHUTTING_DOWN` | `08S01` |
| `INTERNAL_ERROR`, `GENERIC_INTERNAL_ERROR`, `EXTERNAL`, anything else | `HY000` |

## 4. Typical problems

### The driver does not load

The Windows x64 package (the mingw-w64 build, with every dependency linked into `trinoodbc.dll`)
has been installed and loaded on Windows. The Windows rows below were not reproduced one by one;
the *system error 126* row applies to the MSVC/vcpkg build, which ships dependency DLLs.

| Symptom | Fix |
|---|---|
| Windows `IM002 Data source name not found and no default driver specified` | The DSN or driver is registered for the other bitness. Power BI is 64-bit: use `C:\Windows\System32\odbcad32.exe` (64-bit) and a *System* DSN. 32-bit tools need a 32-bit driver build. |
| Windows `Specified driver could not be loaded due to system error 126` | A DLL the driver depends on (libcurl, zstd, lz4, the VC++ runtime) is missing from the driver directory or `PATH`. `packaging/windows/build.ps1` copies the vcpkg DLLs next to `trinoodbc.dll`; install the *Visual C++ Redistributable*. |
| unixODBC `[01000][unixODBC][Driver Manager]Can't open lib '...' : file not found` | Wrong `Driver=` path in `odbcinst.ini`, or a missing shared library: run `ldd libtrinoodbc.so`. |
| macOS `dlopen ... code signature` / `not valid for use in process` | Unsigned or quarantined dylib: `xattr -d com.apple.quarantine libtrinoodbc.dylib`, or build locally. |
| Wide strings come back as garbage / every other character is 0 on macOS | The driver was built for the wrong driver manager (iODBC uses 4-byte `SQLWCHAR`, unixODBC 2-byte). Rebuild with `-DTRINO_ODBC_DRIVER_MANAGER=iodbc` / `unixodbc` to match the application. |
| `IM002` on Unix although the DSN exists | The application reads a different `odbc.ini`: `odbcinst -j` (unixODBC) shows the paths; iODBC uses `~/Library/ODBC/odbc.ini` / `/Library/ODBC/odbc.ini` or `$ODBCINI`. |

### Connecting

| Symptom | Fix |
|---|---|
| `HY000 Host is required` | `Host=` (alias `Server=`) is missing from the DSN and the connection string. |
| `28000 Password authentication requires SSL` (or `Token ...` / `Kerberos ...` / `OAuth2 ...`) | Remove `SSL=0` / use the HTTPS port, or set `AllowInsecurePassword=1` for a development server. See [LDAP-AD.md](LDAP-AD.md). |
| `28000 Certificate authentication requires SSL` | Certificate authentication never works over plain HTTP; remove `SSL=0`. |
| Credentials suddenly go over `http://` / port 8080 | `AllowInsecurePassword=1` turns the automatic TLS off; add `SSL=1` or remove the flag. |
| `28000 User name is required (UID)` | Add `UID=` (Trino needs `X-Trino-User` for `None`, `LDAP`, `JWT`). |
| `28000 Authentication failed (HTTP 401)` | Wrong credentials or token; LDAP bind pattern/group filter; expired JWT. |
| `28000 Access denied (HTTP 403)` | Authenticated but not authorised: access control rules, `X-Trino-User` differs from the mapped principal, impersonation (`SessionUser`) not allowed. |
| `08001` with a certificate message | See [SSL.md](SSL.md). |
| `08001 Could not resolve host` / `Connection refused` | Host name, port, VPN, proxy (`HttpProxy`). |
| `HYT01` | The coordinator does not answer within `ConnectTimeout`; firewall or wrong port (an HTTPS port answered on plain HTTP hangs). |
| `08001 HTTP 404` at connect | `Host`/`Port` point at something that is not a Trino coordinator (e.g. the web UI of a proxy). |
| Kerberos: `28000 ... HTTP 401` with `Negotiate` in the log | No ticket (`klist`), wrong `KerberosServiceName`, a `Host` name that does not match the registered SPN (connect with the canonical name; `KerberosUseCanonicalHostname` currently has no effect). A libcurl without SPNEGO support fails earlier with `HYC00`. |
| OAuth2: `28000 ... timed out` | The browser login was not completed within `ExternalAuthTimeout`; on a headless machine use `ExternalAuthRedirectHandler=print` and open the URL from the log. |
| OAuth2 prompts on every connection | The process-wide token cache is used only when `UID` is set; add `UID=`. |
| `HY000 Invalid value for <Key>: ...` | A malformed or out-of-range option value; the message names the key and the accepted range. |
| `HY000 Invalid character in HTTP header '<name>'` | A value that ends up in a request header (`Source`, `ClientInfo`, `ClientTags`, `TraceToken`, `ExtraHeaders`, ...) contains a control character such as CR/LF/TAB. |

### Running queries

| Symptom | Fix |
|---|---|
| `42000` syntax error on a statement with `?` parameters | The server is older than Trino 431 (`EXECUTE IMMEDIATE`): set `ExplicitPrepare=1`. |
| `42000` for `{fn ...}` | Malformed escape sequence, or a function the translator does not know (it is passed through; Trino then reports the error). `SQLNativeSql` shows the translated SQL. |
| `HYC00` when the application turns autocommit off | Set `EnableTransactions=1` (only useful with transactional connectors) or keep autocommit on. |
| `HYT00` in the middle of a long query | `RequestTimeout` (per HTTP request) or `QueryTimeout`. Raise them; `RequestTimeout=0` disables the per-request limit. |
| `HY008` | The application cancelled (Power BI cancels when a preview is abandoned), or `IdleFetchTimeout` cancelled a result that was not read for that long (the message names `IdleFetchTimeout`). See "Cancelled queries keep running" below. |
| Cancelled queries keep running on the coordinator | See "Cancelled queries keep running" below. |
| `57014 Query exceeded per-node memory limit` etc. | Cluster limits; tune the query or the session properties (`SessionProperties=query_max_memory=...`). |
| `22003` / `01S07` while fetching | The C type bound is too small (e.g. `SQL_C_SLONG` for a bigint) or drops fractions; bind a wider type or `SQL_C_CHAR`. |
| Unicode text shows `?` | Narrow API with `AnsiEncoding=system` on Windows: switch the application to the wide API or set `AnsiEncoding=utf8` if it expects UTF-8. |
| Timestamps shifted by hours | By default the session time zone is `Europe/Istanbul` (a deliberate project default) and `TimestampWithTimeZoneMode=session` delivers zoned timestamps in that zone (UTC+03:00), and Trino evaluates `current_timestamp` / `localtimestamp` there too. Set `TimeZone=system` (OS zone) or another IANA name, `TimestampWithTimeZoneMode=utc` for UTC, `wallclock` to keep the digits as sent or `string` for the original text. |
| `01000` "Time zone '...' is unknown; value returned as wall-clock time" | The zone name of the value is unknown to the OS tz database (`TZDIR`, ICU on Windows); the value is returned as wall clock. |
| `01000` "Session time zone '...' is unknown; value returned in UTC" | The `TimeZone` value (or the OS zone with `TimeZone=system`) cannot be resolved; `session` mode then delivers UTC. Fix `TimeZone` (IANA name such as `Europe/Istanbul`, or `+03:00`). |
| `SQLColumns` returns nothing | Search patterns: `_` and `%` are wildcards; escape with `\` (`SQL_SEARCH_PATTERN_ESCAPE`) or set `SQL_ATTR_METADATA_ID` / `MetadataCaseSensitive=1`. Trino identifiers are lower-case. |
| Spooled results fail with `HYC00 ... encoding` | The server chose an encoding the driver was built without; restrict `QueryDataEncoding=json` or rebuild with zstd/lz4. |
| Spooled results fail with `08S01` on a segment URL | The client cannot reach the spooling storage (object store URL) that the server handed out; fix network access, use `protocol.spooling.retrieval-mode=COORDINATOR_PROXY` on the server, or set `Spooling=0`. |
| Query error *"Requests specifying Server Side Encryption with Customer provided keys must be made over a secure connection"* | Server side: the spooling store is S3-compatible storage reached over plain HTTP (for example MinIO) while segments are SSE-C encrypted; set `fs.segment.encryption=false` in `spooling-manager.properties` (or use HTTPS to the store). |

### Power BI

Power BI runs on Windows only. The driver has been used from Power BI Desktop on Windows x64; the
symptoms below describe how Power BI uses ODBC drivers and were not each reproduced.

| Symptom | Fix |
|---|---|
| `DataSource.Error: ODBC: ERROR [IM002] ...` | See "The driver does not load": 64-bit driver + System DSN. |
| `DataSource.Error: ODBC: ERROR [28000] ...` | Choose *Database* credentials (user name + password = LDAP) or *Windows* (Kerberos) in the credential dialog; *Default or Custom* uses only what the DSN contains. |
| `We couldn't authenticate with the credentials provided` | Same as above; the password is passed as `PWD`, check the DSN does not override `AuthenticationType`. |
| Refresh in the service fails but works in Desktop | The gateway machine needs the driver, a *System* DSN (or a connection string), and network access to the coordinator and, with spooling, to the spooling storage. See [POWERBI.md](POWERBI.md). |
| `The query timed out` | Increase *Command timeout* in the source's advanced options (`SQL_ATTR_QUERY_TIMEOUT`) and `RequestTimeout` in the DSN. |
| Tables show but load slowly | See [PERFORMANCE.md](PERFORMANCE.md); check that filters fold (*View Native Query*). |
| Queries stay `RUNNING` in the Trino UI after a refresh was cancelled | The mashup container was killed before it could cancel, or it abandoned the statement. Set `IdleFetchTimeout=600` and lower `query.client.timeout` on the coordinator; details below and in [POWERBI.md](POWERBI.md#7-cancelling-a-refresh-or-a-preview). |

### Cancelled queries keep running

How the driver stops a query on the server:

* It sends `DELETE <nextUri>` (Trino's cancel request; the query then shows `FAILED` with
  `USER_CANCELED`) whenever a query that is still running is abandoned: `SQLCancel` /
  `SQLCancelHandle` (also when no function is in progress, e.g. between two `SQLFetch` calls; the
  next fetch then returns `HY008`), `SQLCloseCursor`, `SQLFreeStmt(SQL_CLOSE / SQL_DROP)`,
  `SQLFreeHandle` of the statement, connection or environment, `SQLDisconnect`, `SQLMoreResults`,
  re-executing the statement, `SQL_ATTR_MAX_ROWS` reached, and a transfer error of the background
  prefetch. A result the server has finished delivering has nothing left to cancel; `SQLCancel`
  then leaves the cursor readable.
* The `DELETE` is sent from a helper thread with a 5 s budget and one retry on a network error.
  `SQLDisconnect` / `SQLFreeHandle(SQL_HANDLE_DBC)` wait up to 3 s for pending `DELETE`s.
* The driver log shows `cancelling query <id>` / `closing unfinished query <id>` at `LogLevel=3`
  and `DELETE ... -> HTTP <status>` (or the failure) at `LogLevel=4`.

When the application process is killed (for example a Power BI evaluation container terminated
by a refresh timeout or when Power BI Desktop closes), no driver code runs any more and the
coordinator cannot be told. Trino then ends the query when no client has asked for results for
`query.client.timeout` (coordinator `config.properties`, default `5m`). Administrators can lower
it, e.g. `query.client.timeout=2m`; keep it well above the longest gap a healthy client leaves
between two result requests (while the prefetch queue is full the driver does not poll the
coordinator, so a longer pause of the application makes Trino fail the query as
`ABANDONED_QUERY`).

When the application keeps its handles but stops reading (it neither cancels nor closes), set
`IdleFetchTimeout=<seconds>` (alias `AbandonedQueryTimeout`, default `0` = off; `600` is a good
value for Power BI): once no call was made on the statement for that long while its query is still
running on the server, the driver cancels the query and the next fetch returns `HY008`.

To check a query from the command line:
`curl -H "X-Trino-User: <user>" http://<coordinator>:8080/v1/query/<queryId>` shows `"state"` and
`"errorCode"`.

## 5. Reporting a problem

Include: the driver log at `LogLevel=4` (secrets are masked automatically), the SQLSTATE and
message, the Trino version (`SELECT version()`), the driver manager (Windows / unixODBC / iODBC
and its version), the operating system, and the connection string with the password removed.
