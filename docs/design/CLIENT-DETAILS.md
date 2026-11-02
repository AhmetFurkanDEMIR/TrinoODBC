# Client module design notes (http_client / trino_client / oauth2 / spool_decoder / json_decoder)

Module-private headers: `src/client/http_client.h`, `src/client/oauth2.h`, `src/client/spool_decoder.h`,
`src/result/json_decoder.h`. Public interface: `src/client/trino_client.h` (shared with the ODBC layer; change it
only with care).

## libcurl usage
* `TrinoClient::globalInit()` -> `curl_global_init(CURL_GLOBAL_ALL)` under `std::call_once`. Never call curl_global_cleanup
  (the DLL may be unloaded from DllMain; leaking the global state is the accepted practice).
* One `CURL*` easy handle per `HttpClient` object; an `HttpClient` is used by one thread at a time. TrinoClient owns a
  `CURLSH*` share (DNS, SSL_SESSION, CONNECT) with mutex lock callbacks; every HttpClient of the connection joins it.
* Common options: CURLOPT_NOSIGNAL=1, CURLOPT_CONNECTTIMEOUT=connectTimeoutSec, CURLOPT_TIMEOUT=requestTimeoutSec (0 = none),
  CURLOPT_USERAGENT=kUserAgent, CURLOPT_ACCEPT_ENCODING="" when compression, CURLOPT_TCP_KEEPALIVE=1, CURLOPT_HTTP_VERSION
  default, CURLOPT_FOLLOWLOCATION=0, CURLOPT_XFERINFOFUNCTION checking an `std::atomic<bool>* cancel` (return 1 -> abort),
  CURLOPT_HEADERFUNCTION collecting response headers (case-insensitive multimap), CURLOPT_WRITEFUNCTION appending to a
  `simdjson::padded_string`-compatible buffer (std::string reserved with SIMDJSON_PADDING).
* TLS: CURLOPT_SSL_VERIFYPEER = (verification != None); CURLOPT_SSL_VERIFYHOST = (verification == Full ? 2 : 0);
  CURLOPT_CAINFO/CURLOPT_CAPATH when given; on Windows with useSystemTrustStore set CURLOPT_SSL_OPTIONS |= CURLSSLOPT_NATIVE_CA
  (guard with `#ifdef CURLSSLOPT_NATIVE_CA`); CURLOPT_SSLVERSION = CURL_SSLVERSION_TLSv1_2 or TLSv1_3; client cert:
  CURLOPT_SSLCERT (+ CURLOPT_SSLCERTTYPE "P12" for .p12/.pfx), CURLOPT_SSLKEY, CURLOPT_KEYPASSWD.
* Auth: Ldap -> CURLOPT_HTTPAUTH=CURLAUTH_BASIC, CURLOPT_USERNAME/CURLOPT_PASSWORD; Jwt -> header `Authorization: Bearer <token>`
  (token from AccessToken or AccessTokenFile read at connect, trimmed); Kerberos -> CURLOPT_HTTPAUTH=CURLAUTH_NEGOTIATE,
  CURLOPT_USERPWD=":", CURLOPT_SERVICE_NAME=kerberosServiceName, CURLOPT_GSSAPI_DELEGATION=(delegation?CURLGSSAPI_DELEGATION_FLAG:
  CURLGSSAPI_DELEGATION_NONE); Certificate -> client cert only; OAuth2 -> Bearer token obtained by the external flow (below).
  Ldap/Jwt/Kerberos/OAuth2 over plain http: refuse at connect with 28000 unless allowInsecurePassword (Ldap only).
* Proxies: CURLOPT_PROXY = "http://host:port" or "socks5h://host:port", CURLOPT_PROXYUSERPWD.
* Errors: curl failure -> HttpError("08S01" / "HYT00" for CURLE_OPERATION_TIMEDOUT / "08001" for connect failures during connect(),
  message includes curl_easy_strerror + URL host). HTTP 401/403 -> HttpError("28000", ...) (403 message "Access denied");
  other non-2xx -> HttpError("08S01", "HTTP <code>: <body excerpt>") except 503/502/504 which are retried (see below).
  Aborted by cancel flag -> CancelledError.
* Retries: for `GET nextUri` and `POST /v1/statement`: on 503 or curl connect/recv/send errors, sleep 100ms * 2^attempt (cap 2s,
  + jitter from a per-client counter rather than a random generator), up to maxRetries; then throw the last error.

## OAuth2 external authentication (oauth2.cpp)
The first request answered with 401 and a `WWW-Authenticate: Bearer x_redirect_server="<url>", x_token_server="<url>"`
header starts the flow: RedirectHandler::Open launches the browser (`open` on macOS, `xdg-open` on Linux, `ShellExecuteW` on
Windows) — Print only logs the URL at Info level; then poll `GET x_token_server` (with `Authorization: Bearer` absent) every
~1s until externalAuthTimeoutSec: 200 body `{"token":"..."}` -> done; body `{"nextUri":"..."}` -> poll that URI; body
`{"error":"..."}` or 4xx -> DriverError 28000. Tokens cached process-wide keyed by baseUrl+user; on a later 401 with a cached
token, drop it and run the flow once more. `class OAuth2Authenticator` provides with `std::string obtainToken(HttpClient&, const
std::string& challenge)` and the header parser `bool parseBearerChallenge(std::string_view header, std::string* redirect,
std::string* tokenServer)` (unit-tested).

## Statement protocol (trino_client.cpp)
Headers per ARCHITECTURE.md §7. Build them from ConnectionConfig + SessionState under the session mutex. Values in
`X-Trino-Session`, `X-Trino-Extra-Credential`, `X-Trino-Prepared-Statement` are `key=urlEncode(value)` joined by ",".
`X-Trino-Client-Capabilities: PARAMETRIC_DATETIME,SESSION_AUTHORIZATION`. `X-Trino-Transaction-Id`: the session transaction
id, or `NONE` when opts.startTransaction, otherwise omitted. `X-Trino-Query-Data-Encoding` only when spooling is enabled,
`queryDataEncoding()` non-empty and !opts.internal. `X-Trino-Original-User` (sessionUser) when set. `X-Trino-Source`,
`X-Trino-Client-Info`, `X-Trino-Client-Tags`, `X-Trino-Trace-Token`, `X-Trino-Time-Zone` (resolveSessionTimeZone(config.timeZone):
default kDefaultSessionTimeZone = Europe/Istanbul, `system` -> TimeZoneDb::systemZoneName(), omitted when empty), `X-Trino-Language`, `X-Trino-Role`, `X-Trino-Catalog`, `X-Trino-Schema`,
`X-Trino-Path`, `X-Trino-User` (config.user; for Kerberos/OAuth2/Certificate it may be empty -> omit).
Apply every `X-Trino-Set-*` / `X-Trino-Clear-*` / `X-Trino-Added-Prepare` / `X-Trino-Deallocated-Prepare` /
`X-Trino-Started-Transaction-Id` / `X-Trino-Clear-Transaction-Id` / `X-Trino-Set-Authorization-User` /
`X-Trino-Reset-Authorization-User` response header to SessionState after every response (values URL-decoded where Trino encodes).

`execute()`: POST, then follow nextUri on the calling thread until `columns` is known and (a data page arrived or nextUri is
absent) or the query failed/finished. Decode `data` into a Page immediately (typed by the ResultColumns). `updateType`/
`updateCount` recorded. Then, unless `opts.internal` or the query is already finished, start the prefetch thread: it keeps
following nextUri (appending `targetResultSize=<N>MB` as a query parameter, preserving existing ones), decoding pages (and
spooled segments) into a bounded queue (`prefetchPages` pages or `maxPrefetchBytes` bytes, whichever first). `nextPage()` pops.
Errors on the thread are stored as `std::exception_ptr` and rethrown from nextPage(). `cancel()` sets the cancel flag,
aborts in-flight transfers and sends `DELETE nextUri` from a detached helper (5 s timeout, one retry) when the server is still producing rows.
`close()` cancels if unfinished, joins the thread, frees pages. Destructor calls close(). `maxRows` stops after N rows.
Server error object -> TrinoQueryError(sqlstate = error.sqlState if present else Diag::sqlstateForTrinoError(errorName,
errorType), message, errorCode, errorName, errorType, queryId, line, column). `warnings[]` are accumulated.

## Spooled protocol (spool_decoder.cpp + json_decoder.cpp)
`data` as object: `{ "encoding": "json+zstd", "metadata": {...}, "segments": [ {"type":"inline","data":"<base64>",
"metadata":{"segmentSize":N,"rowOffset":R,"rowsCount":C,"uncompressedSize":U}}, {"type":"spooled","uri":"...","ackUri":"...",
"headers":{"name":["v"]},"metadata":{...}} ] }`. Decode: base64 (inline) or GET uri with the given headers (no Trino auth
headers; a dedicated HttpClient per worker; up to maxParallelSegments workers) -> bytes -> decompress (zstd/lz4 block:
`json+zstd` = ZSTD_decompress to uncompressedSize; `json+lz4` = LZ4_decompress_safe to uncompressedSize; `json` = raw) ->
a JSON array of rows -> Page via `decodeRowsJson`. Deliver pages in rowOffset order. After decoding a spooled segment send
`DELETE ackUri` (best effort, fire-and-forget on the worker). `std::string decodeSegmentBytes(std::string_view
encoding, std::string_view bytes, size_t uncompressedSize)` is exposed for unit tests (throws DriverError HY000 on corrupt data, HYC00
for an encoding not compiled in). `TrinoClient::queryDataEncoding()` = config list filtered by TRINO_ODBC_HAVE_ZSTD / _LZ4
("json" always available).

## JSON decoding (json_decoder.cpp with simdjson on-demand)
* `struct StatementResponse` with id, infoUri, nextUri, partialCancelUri, `std::vector<ResultColumn> columns` + `bool
  columnsPresent`, `std::unique_ptr<Page> page` (null when no data), `bool dataIsSpooled` + `SpooledData spooled`,
  QueryStats stats, `std::optional<TrinoError> error` (message, errorCode, errorName, errorType, sqlState, line, column),
  warnings, updateType, `std::optional<int64_t> updateCount`.
* `StatementResponse parseStatementResponse(std::string& jsonWithPadding, const std::vector<ResultColumn>* knownColumns,
  const TypeMappingOptions&)`; `std::unique_ptr<Page> decodeRowsJson(std::string& rowsJsonWithPadding, const
  std::vector<ResultColumn>&)`.
* Cell decoding by StorageKind: Bool <- JSON bool; Int64 <- number (also accept string digits); Double <- number or the strings
  "NaN"/"Infinity"/"-Infinity"; Binary <- base64 string; String <- string as-is; complex (array/map/row/unknown) <-
  `raw_json()` of the element (compacted only if needed, never re-serialized). JSON null -> appendNull().
* Column types come from `columns[].type` via `TrinoType::parse` (typeSignature is not needed).
* Buffers passed to simdjson must have SIMDJSON_PADDING spare bytes (std::string capacity), one `simdjson::ondemand::parser`
  reused per thread/handle.
