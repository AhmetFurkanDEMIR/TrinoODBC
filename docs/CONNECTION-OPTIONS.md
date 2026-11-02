# Connection options

Every DSN entry and connection-string key understood by the Trino ODBC Driver. The same keys work
in a DSN (`odbc.ini`, the Windows registry, the Windows setup dialog) and in the connection string
passed to `SQLDriverConnect` / `SQLBrowseConnect`, and in the `Advanced options` record of the
Power Query connector.

## Grammar and precedence

* Keys are **case-insensitive**. The first name in each row below is the canonical name; the
  aliases are accepted everywhere. Aliases exist so that DSNs written for the Starburst/Simba
  driver or copied from a Trino JDBC URL keep working.
* Connection strings follow the ODBC grammar: `Key=Value;Key2=Value2`. A value that contains `;`
  (or leading/trailing blanks, `{`, `}`, `=`, `,`, quotes) must be enclosed in braces:
  `SessionProperties={query_max_run_time=1h;join_distribution_type=BROADCAST}`. A literal `}`
  inside braces is written `}}`. Single- or double-quoted values are accepted too (`"a;b"`,
  a doubled quote is a literal quote). Fragments without `=` are ignored.
* When a key appears twice in a connection string the **first occurrence wins**.
* `DSN=name` loads every entry of that DSN section, but **connection-string values win over DSN
  values**. An empty DSN entry (`Port=`) means "not set".
* Unknown keys are not an error: they are kept, logged at level 2 (warn), reported as a warning
  record `01S00` ("Invalid connection string attribute ignored", the connect call returns
  `SQL_SUCCESS_WITH_INFO`) and echoed back in the out connection string.
* `DSN=name` naming a section that does not exist fails with `IM002`, unless the string also
  contains `Driver=`: then the driver logs a warning and connects with the inline values only
  (some tools pass a placeholder DSN together with every value).
* Booleans accept `1/0`, `true/false`, `yes/no`, `on/off`.
* Secrets (`PWD`, `ClientKeyPassword`, `AccessToken`, `ExtraCredentials`, `ProxyPassword`) are
  never written to the log. `PWD`, `ClientKeyPassword` and `ProxyPassword` are taken verbatim (a
  password may end in a space: brace-quote it in a connection string, `PWD={secret }`, because an
  unbraced value is trimmed by the parser); every other value is trimmed.
* Malformed or out-of-range values (`Port=70000`, `SSL=maybe`, `TargetResultSize=abc`) make the
  connect call fail with `HY000` and a message of the form
  `Invalid value for Port: '70000' (expected an integer between 1 and 65535)`.
* The connection string returned by `SQLDriverConnect` (the "out" string) contains every key that
  was given (under its canonical name) plus the derived `Host`, `Port`, `SSL`, `UID` (when set)
  and `AuthenticationType` (when not `None`), including the password exactly as supplied, so an
  application can reconnect with it. `AllowSelfSignedServerCert` / `AllowHostNameCNMismatch` are
  folded into `SSLVerification`; unknown keys are appended as given.

## Derived defaults

Applied after all keys have been read:

| Situation | Result |
|---|---|
| `AuthenticationType` not given | `LDAP` when `PWD` is set; `JWT` when `AccessToken` or `AccessTokenFile` is set; otherwise `None` |
| `SSL` not given | `1` when `Port=443`, or when the resolved authentication type is not `None` **and** `AllowInsecurePassword` is not `1`; otherwise `0`. An explicit `SSL=0` or `SSL=1` is never changed. |
| `Port` not given | `443` when SSL is on, otherwise `8080` (Trino's HTTPS listener is often `8443`: set `Port` explicitly) |
| `Source` empty | `trino-odbc` |
| `KerberosServiceName` empty | `HTTP` |
| `QueryDataEncoding` empty | `json+zstd,json+lz4,json` |
| `TimeZone` not given or empty | `Europe/Istanbul` (project default, see below); `TimeZone=system` resolves to the OS zone when the session starts |

### Default time zone: Europe/Istanbul

The driver uses Turkey time by default. This is a deliberate project default, not something
derived from the machine: with no `TimeZone` key the driver sends `X-Trino-Time-Zone: Europe/Istanbul`
(so `current_timestamp`, `localtimestamp` and timestamp casts are evaluated in Istanbul time on the
server) and delivers every `timestamp with time zone` value as the Istanbul wall clock
(`TimestampWithTimeZoneMode=session`). Europe/Istanbul has been UTC+03:00 all year since 2016, so
`2024-03-10 16:30:00 UTC` is returned as `2024-03-10 19:30:00`.

To change it:

| Wanted | Setting |
|---|---|
| another zone for the session and the values | `TimeZone=Europe/Berlin` (any IANA name or `+HH:MM`) |
| the zone of the computer the driver runs on (the behaviour before this default) | `TimeZone=system` |
| zoned timestamps in UTC regardless of the session zone | `TimestampWithTimeZoneMode=utc` |
| zoned timestamps with their original digits / text | `TimestampWithTimeZoneMode=wallclock` / `string` |

`Europe/Istanbul` resolves everywhere: from the TZif database on macOS and Linux, through ICU or
the registry rule of "Turkey Standard Time" on Windows, and from a compiled-in fixed +03:00 offset
when none of those is available.

## Validation

Checked before any network request. The SQLSTATE is what `SQLGetDiagRec` returns.

| Rule | SQLSTATE |
|---|---|
| Every value must parse and be inside the range listed for its key below (checked while the connection string / DSN is read) | `HY000` |
| `Host` is required | `HY000` |
| `LDAP` requires `UID` and `PWD` | `28000` |
| `LDAP` over plain HTTP (`SSL=0`) requires `AllowInsecurePassword=1` ("Password authentication requires SSL ...") | `28000` |
| `JWT` requires `AccessToken` or `AccessTokenFile`; over plain HTTP it requires `AllowInsecurePassword=1` ("Token authentication requires SSL ...") | `28000` |
| `Kerberos` / `OAuth2` over plain HTTP require `AllowInsecurePassword=1` | `28000` |
| `Certificate` requires `ClientCert` and always requires SSL (`AllowInsecurePassword` does not apply) | `28000` |
| `UID` is required for `None`, `LDAP` and `JWT` (Kerberos, OAuth2 and certificates identify the user server-side) | `28000` |
| `HttpProxy` and `SocksProxy` are mutually exclusive | `HY024` |
| `SSLVerification=None` with SSL on | not an error; a warning is written to the log at every connect |

Because of the derived defaults, the "requires SSL" errors only occur with an explicit `SSL=0`
(and no `AllowInsecurePassword=1`), and for `Certificate` also when `AllowInsecurePassword=1` is
set without `SSL=1` (that flag turns the automatic TLS off, and certificates never work without
it). `JWT` with an `AccessTokenFile` that cannot be read, or that is empty, fails at
connect with `28000`.

Header values sent to Trino (`Source`, `ClientInfo`, `ClientTags`, `TraceToken`, `ExtraHeaders`,
...) must not contain control characters (CR, LF, TAB, other bytes below 0x20, DEL): such a value
is refused when the request is built, with `HY000` "Invalid character in HTTP header '<name>'".

## Key reference

Types: *string*, *int*, *bool*, *enum*, *path*, *list*. "Header" names the Trino request header the
value ends up in.

### Identity

| Key (aliases) | Type / default | Description |
|---|---|---|
| `DSN` | string | Name of a DSN section to load. Standard ODBC key. |
| `Driver` | string | Driver name (`Trino ODBC Driver`) or path. Standard ODBC key; used by the driver manager, echoed by the driver. |
| `Description` | string | Free text stored in the DSN. |

### Endpoint

| Key (aliases) | Type / default | Description |
|---|---|---|
| `Host` (`Server`, `HostName`) | string, **required** | Trino coordinator host name or IP. IPv6 literals may be given bare (`::1`) or bracketed (`[::1]`). |
| `Port` | int, `8080` (`443` when SSL is on) | Coordinator HTTP(S) port. Range 1..65535. `Port=443` also switches SSL on when `SSL` is not given. |
| `Catalog` (`Database`) | string | Default catalog. Header `X-Trino-Catalog`; also the initial value of `SQL_ATTR_CURRENT_CATALOG` / `SQL_DATABASE_NAME`. |
| `Schema` | string | Default schema. Header `X-Trino-Schema`. |

Example: `Driver={Trino ODBC Driver};Host=trino.example.com;Port=8080;Catalog=tpch;Schema=tiny;UID=analyst`

### Authentication

| Key (aliases) | Type / default | Description |
|---|---|---|
| `UID` (`User`, `Username`) | string | Trino user name, sent as `X-Trino-User`. Required unless Kerberos, OAuth2 or a client certificate identifies the user. For LDAP/AD it is also the Basic-auth user name. |
| `PWD` (`Password`) | string | Password for `LDAP` (HTTP Basic over TLS). Taken verbatim. |
| `AuthenticationType` (`AuthMech`, `Auth`) | enum, derived (see above) | Case-insensitive. `None` (`0`, `noauth`, `anonymous`, `No Authentication`), `LDAP` (`1`, `Password`, `Basic`, `UsernamePassword`, `UserPassword`, `Username/Password`, `Username and Password`), `Kerberos` (`2`, `krb5`, `gssapi`, `spnego`), `JWT` (`3`, `Token`, `AccessToken`, `bearer`), `OAuth2` (`4`, `ExternalAuthentication`, `External`, `oauth`, `browser`), `Certificate` (`5`, `cert`, `clientcert`, `mtls`). |
| `AllowInsecurePassword` | bool, `0` | Permit `LDAP`, `JWT`, `Kerberos` and `OAuth2` credentials over plain `http://` (never `Certificate`). It also turns off the automatic TLS: with `AllowInsecurePassword=1` and no `SSL` key the connection uses `http://` (port `8080` unless `Port` is given); set `SSL=1` to keep TLS. The Trino server must also accept it (`http-server.authentication.allow-insecure-over-http=true`). Development only. |
| `AccessToken` (`JWT`, `AuthToken`, `Token`) | string | Bearer token for `JWT`. Sent as `Authorization: Bearer <token>`. Value is trimmed. |
| `AccessTokenFile` | path | File whose (trimmed) content is the bearer token, read at every connect. Use it for rotating tokens. When both are given, the file wins over `AccessToken`. |
| `KerberosServiceName` (`KerberosRemoteServiceName`, `KrbServiceName`) | string, `HTTP` | Service part of the SPNEGO principal `<name>/<host>` (`CURLOPT_SERVICE_NAME`). |
| `KerberosDelegation` | bool, `0` | Forward the Kerberos TGT to Trino (`CURLOPT_GSSAPI_DELEGATION`). |
| `KerberosUseCanonicalHostname` | bool, `1` | Accepted and echoed for compatibility, but currently **has no effect**: the SPN host is whatever libcurl's GSS-API/SSPI layer derives from `Host`. Connect with the name the SPN was registered for. |
| `ExternalAuthTimeout` (`OAuth2Timeout`) | int seconds, `120` | How long the driver waits for the browser login to complete. Range 1..86400. |
| `ExternalAuthRedirectHandler` | enum, `open` | `open` (`browser`, `launch`): launch the system browser with the login URL (`ShellExecute` on Windows, `open` on macOS, `xdg-open` on Linux; only `http(s)` URLs are ever launched). `print` (`log`, `none`): do not launch anything. In both modes the URL is written to the log at level 3 (info), so an operator can open it. |

The Kerberos mode needs a libcurl built with SPNEGO/Negotiate support (SSPI on Windows, GSSAPI on
Linux/macOS; `curl --version` must list `SPNEGO` under *Features*) and a valid ticket cache
(`kinit`, or a domain logon on Windows). Without it the connect fails with `HYC00` ("libcurl was
built without GSS-API/SPNEGO support"). Kerberos, OAuth2 and `Certificate` authentication are
implemented but have not been exercised against a real KDC / identity provider in this project;
the verified modes are `None` and `LDAP` (password) against Trino 483.

### TLS

See [SSL.md](SSL.md) for the full discussion.

| Key (aliases) | Type / default | Description |
|---|---|---|
| `SSL` (`UseSSL`, `Encrypt`, `Secure`) | bool, derived | `1` = `https://`. When not given: on for `Port=443`, and on for every authentication type other than `None` unless `AllowInsecurePassword=1`. An explicit value always wins. |
| `SSLVerification` (`SSLMode`) | enum, `Full` | `Full` (`verify-full`, `verify_full`, `verifyfull`, `on`, `true`, `yes`, `1`): verify the certificate chain and the host name. `CA` (`verify-ca`, `verify_ca`, `verifyca`, `chain`): chain only, no host-name check. `None` (`off`, `false`, `no`, `0`, `disable`, `disabled`, `insecure`): trust everything (logged warning). |
| `AllowSelfSignedServerCert` | bool | Simba-compatible flag: `1` sets `SSLVerification=None`; `0` changes nothing. |
| `AllowHostNameCNMismatch` | bool | Simba-compatible flag: `1` lowers `SSLVerification` from `Full` to `CA`. An explicit `SSLVerification` in the connection string is never undone by these flags from a DSN. |
| `TrustedCerts` (`SSLCA`, `CAFile`, `SSLTrustStorePath`) | path | PEM bundle of trusted CA certificates (`CURLOPT_CAINFO`). |
| `CAPath` (`SSLCAPath`) | path | Directory of hashed PEM certificates (`c_rehash` layout, `CURLOPT_CAPATH`). |
| `UseSystemTrustStore` | bool, `1` | Windows: also trust the Windows certificate store (`CURLSSLOPT_NATIVE_CA`, only when the libcurl the driver is built against defines it). The Windows x64 package (libcurl with Schannel) has been verified over HTTPS on Windows. macOS/Linux: the key has no effect; libcurl's default CA bundle is used. |
| `ClientCert` (`SSLCert`, `SSLKeyStorePath`) | path | Client certificate for mutual TLS / `Certificate` authentication: PEM, or PKCS#12 when the file name ends in `.p12` / `.pfx` (case-insensitive). |
| `ClientKey` (`SSLKey`) | path | PEM private key (not needed for PKCS#12). Ignored without `ClientCert`. |
| `ClientKeyPassword` (`SSLKeyPassword`, `SSLKeyStorePassword`) | string | Passphrase of the private key / PKCS#12 file (taken verbatim). Ignored without `ClientCert`. |
| `MinTLSVersion` | enum, `1.2` | `1.2` (`tlsv1.2`, `tls1.2`, `tls12`, `12`) or `1.3` (`tlsv1.3`, `tls1.3`, `tls13`, `13`). Minimum protocol version offered to the server (`CURLOPT_SSLVERSION`). |

### Session

| Key (aliases) | Type / default | Description |
|---|---|---|
| `Source` (`ApplicationName`) | string, `trino-odbc` | `X-Trino-Source`; shows up in the Trino UI and `system.runtime.queries`. |
| `ClientInfo` | string | `X-Trino-Client-Info`. |
| `ClientTags` | list (`,` or `;`) | `X-Trino-Client-Tags`, e.g. `ClientTags=odbc,reporting` (sent comma-separated). Used by resource groups. |
| `TraceToken` | string | `X-Trino-Trace-Token`. |
| `TimeZone` (`TimeZoneID`) | IANA name, offset or `system`; `Europe/Istanbul` | Session time zone, sent as `X-Trino-Time-Zone` and used by `TimestampWithTimeZoneMode=session`. **Project default: `Europe/Istanbul`** (Turkey time, UTC+03:00 all year since 2016) when the key is absent or empty. `system` selects the operating-system zone (`$TZ`, `/etc/localtime`, Windows time-zone settings; the header is omitted when it cannot be determined). Any IANA name (`Europe/Berlin`) or fixed offset (`+05:30`) is used as given. |
| `Locale` (`Language`) | string | `X-Trino-Language`, e.g. `tr-TR`. |
| `SessionProperties` | `k=v;k2=v2` (`;` or `,`) | `X-Trino-Session` (values URL-encoded). Catalog properties use `catalog.property=value`. Brace-quote the value: `SessionProperties={query_max_run_time=1h;hive.parquet_use_column_names=true}`. An entry without `=` is an error (`HY000`). |
| `ExtraCredentials` | `k=v;k2=v2` (`;` or `,`) | `X-Trino-Extra-Credential` (e.g. for connectors that pass through credentials). Treated as a secret. |
| `Roles` | `catalog=spec;...` (`;` or `,`) | `X-Trino-Role`. Each entry is `catalog=ROLE{name}`, `catalog=name` (same thing), `catalog=ALL` or `catalog=NONE`; an entry without `catalog=` applies to `system`. Example: `Roles={hive=ROLE{analyst};system=ALL}`. |
| `SessionUser` | string | Run queries as this user while authenticating as `UID`: the driver sends `SessionUser` as `X-Trino-User` and `UID` as `X-Trino-Original-User`. Trino must allow the impersonation. |
| `ExtraHeaders` | `Name:value;Name2:value2` | Added to every request, including spooled-segment downloads (`Name=value` is accepted too; only `;` separates entries, so values may contain commas). Names must not be empty or contain blanks (`HY000` while parsing); names or values with control characters are refused with `HY000` when the request is built. |

### Network

| Key (aliases) | Type / default | Description |
|---|---|---|
| `HttpProxy` (`Proxy`) | `host:port` or URL | HTTP proxy (`CURLOPT_PROXY`; `http://` is prepended when no scheme is given). |
| `SocksProxy` | `host:port` or URL | SOCKS5 proxy with remote DNS (`socks5h://` is prepended when no scheme is given). Mutually exclusive with `HttpProxy`. |
| `ProxyUser`, `ProxyPassword` | string | Proxy credentials (used only when `ProxyUser` is set; `ProxyPassword` is taken verbatim). |
| `ConnectTimeout` (`LoginTimeout`) | int seconds, `30` | TCP + TLS connect timeout of every request (`CURLOPT_CONNECTTIMEOUT`). A non-zero `SQL_ATTR_LOGIN_TIMEOUT` set before connecting overrides it; setting that attribute after the connection is open is accepted and ignored. Range 0..86400 (0 = libcurl's built-in default of 300 s). |
| `RequestTimeout` (`Timeout`) | int seconds, `60` | Timeout of each HTTP request (each page / segment fetch), 0 = none. Range 0..604800. |
| `QueryTimeout` | int seconds, `0` | Default `SQL_ATTR_QUERY_TIMEOUT`: the query is cancelled (SQLSTATE `HYT00`) when it runs longer. 0 = unlimited. Range 0..2592000. |
| `IdleFetchTimeout` (`AbandonedQueryTimeout`) | int seconds, `0` | Protects the coordinator from abandoned cursors: when a statement's query is still running on the server (the driver is still downloading or waits for the application to make room in the prefetch queue) and the application has not called the driver on that statement (`SQLFetch`, `SQLGetData`, any other statement function) for this long, the driver cancels the query (`DELETE nextUri`, the query ends as `USER_CANCELED`) and the next fetch on the cursor returns `HY008` ("... did not fetch from the open result for N s (IdleFetchTimeout)"). Results the server has finished delivering are never touched, and time spent inside a driver call (a fetch waiting for data) does not count. It matters most for queries that are still computing: the driver keeps polling those, which keeps them alive on the coordinator, whereas a query whose prefetch queue is full is not polled and Trino's `query.client.timeout` (default 5 min) ends it anyway. An application that keeps several results open and reads them one after another needs a value above the time it spends on the others. 0 = off. Recommended for Power BI: `600`. Range 0..2592000. |
| `MaxRetries` (`Retries`) | int, `5` | Retries for HTTP 502/503/504 and transient transport errors, with back-off 100 ms, 200 ms, 400 ms, ... capped at 2 s, plus up to 99 ms jitter. Resolve/connect/TLS-handshake failures are retried for every request; errors after the request was sent (reset, empty reply, HTTP/2 stream errors) only for idempotent requests (`GET nextUri`, segment downloads), never for the `POST` that starts a query. HTTP 401/403 are never retried. Range 0..1000. |
| `Compression` (`EnableCompression`) | bool, `1` | Send `Accept-Encoding` with every encoding the linked libcurl can decode (typically gzip and deflate, plus br/zstd when libcurl was built with them) and decompress transparently. |

### Result transfer

See [PERFORMANCE.md](PERFORMANCE.md).

| Key (aliases) | Type / default | Description |
|---|---|---|
| `Spooling` (`SpoolingEnabled`) | bool, `1` | Send `X-Trino-Query-Data-Encoding` so a server with the spooled protocol enabled returns results as segments (Trino 466+). Servers without spooling ignore the header. Internal queries (catalog functions, `SELECT version()`, `DESCRIBE`) always use the classic protocol. |
| `QueryDataEncoding` | list, `json+zstd,json+lz4,json` | Preferred segment encodings, in order (case-insensitive, duplicates dropped). Only `json`, `json+zstd`, `json+lz4` are valid (anything else: `HY000`). Codecs not compiled into the driver are dropped at connect (`json` is always available). |
| `TargetResultSize` | int MB, `16` | `targetResultSize=<N>MB` appended to every `nextUri` of application queries. Accepted 1..1048576; values above 128 are clamped to 128 with a logged warning. |
| `PrefetchPages` | int, `4` | Pages the background fetcher may queue ahead of the application. Range 1..100000. |
| `MaxPrefetchBytes` (`PrefetchBufferMB`) | int MB, `64` | Memory cap of the prefetch queue (decoded pages); the fetcher pauses when either this or `PrefetchPages` is reached (a single page always fits into an empty queue). Range 1..1048576. |
| `MaxParallelSegments` | int, `4` | Worker threads per query that download and decode spooled segments; at most this many decoded segments wait for the consumer. Range 1..256. |

### Type mapping

See [TYPES.md](TYPES.md).

| Key (aliases) | Type / default | Description |
|---|---|---|
| `UseUnicodeSqlCharacterTypes` (`UnicodeTypes`) | bool, `1` | Report character columns as `SQL_WCHAR` / `SQL_WVARCHAR` / `SQL_WLONGVARCHAR` (`1`) or `SQL_CHAR` / `SQL_VARCHAR` / `SQL_LONGVARCHAR` (`0`). |
| `DefaultStringColumnLength` (`MaxVarcharSize`, `StringColumnLength`) | int, `65535` | Column size reported for unbounded `varchar`, `json`, `varbinary` and complex types (array/map/row). Range 1..2147483647. |
| `UnboundedVarcharAsLongVarchar` | bool, `0` | Report unbounded `varchar` (and `varbinary`) as `SQL_(W)LONGVARCHAR` / `SQL_LONGVARBINARY` instead of `SQL_(W)VARCHAR` / `SQL_VARBINARY`. |
| `TimestampWithTimeZoneMode` (`TimestampTZMode`) | enum, `session` | `session` (`local`, `sessiontz`; **default**): convert `timestamp with time zone` from its own zone/offset to the session time zone (`TimeZone`, default `Europe/Istanbul`) and expose that wall clock as `SQL_TYPE_TIMESTAMP` (text without zone suffix). `utc`: convert to UTC instead (the default before the session mode existed). `wallclock` (`wall-clock`): keep the wall-clock digits as sent, drop the zone. `string` (`text`): expose the original text (`2024-03-10 02:30:00.123 America/New_York`) as a character column. `local` used to be an alias of `wallclock`; it now means `session`. |
| `TimeWithTimeZoneMode` | enum, `string` | Same four modes for `time with time zone`; the default stays `string` (a time of day with an offset has no date, so `session` shifts it by the session zone's offset at the current instant). |
| `IntervalMode` | enum, `string` | `string` (`text`): intervals are character columns (`1-2`, `3 04:05:06.789`). `interval` (`native`, `struct`): `SQL_INTERVAL_YEAR_TO_MONTH` / `SQL_INTERVAL_DAY_TO_SECOND` with `SQL_INTERVAL_STRUCT` as default C type. |
| `UuidAsGuid` | bool, `0` | `1`: `uuid` is `SQL_GUID` (16-byte `SQLGUID` default C type). `0`: `SQL_(W)VARCHAR(36)`. |
| `ComplexTypesAsJson` | bool, `1` | array/map/row values are delivered as their JSON text. Reserved: the value is parsed and echoed, but the driver always does this. |
| `DecimalAsDouble` | bool, `0` | Report `decimal(p,s)` as `SQL_DOUBLE` (default C type `SQL_C_DOUBLE`) for tools without `SQL_NUMERIC_STRUCT` support. The value is still kept as exact text; converting it to a double loses precision beyond 15-17 digits. |

### Behaviour

| Key (aliases) | Type / default | Description |
|---|---|---|
| `ExplicitPrepare` | bool, `0` | `0`: parameters are executed with `EXECUTE IMMEDIATE '<sql>' USING <literals>` (Trino 431+). `1`: the SQL is sent as an `X-Trino-Prepared-Statement` header and executed with `EXECUTE <name> USING ...` (works with older servers). |
| `MetadataCaseSensitive` (`AssumeLiteralNamesInMetadataCalls`) | bool, `0` | Treat the catalog/schema/table/column arguments of `SQLTables`, `SQLColumns` etc. as literal identifiers (`=`) instead of search patterns (`LIKE ... ESCAPE '\'`). Same effect as `SQL_ATTR_METADATA_ID=SQL_TRUE`. |
| `EnableTransactions` | bool, `0` | `0`: `SQL_TXN_CAPABLE=SQL_TC_NONE`; turning autocommit off on an open connection returns `HYC00` (requested before connecting, it is reset to on with a `01S02` warning at connect). `1`: `SQL_TC_DML`; with autocommit off the driver starts a Trino transaction and `SQLEndTran` sends `COMMIT` / `ROLLBACK`. Only meaningful with connectors that support transactions. |
| `ValidateConnection` | bool, `1` | Run `SELECT version()` at connect to verify credentials and fill `SQL_DBMS_VER`. `0` skips the round trip; `SQL_DBMS_VER` is then `00.00.0000`. `OAuth2` without a cached token still sends the probe, so the browser login happens during connect. |
| `ReadOnly` | bool, `0` | Default `SQL_ATTR_ACCESS_MODE`. When read-only, statements starting with `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `CREATE`, `DROP`, `ALTER`, `COMMENT`, `GRANT`, `REVOKE`, `DENY`, `ANALYZE`, `REFRESH` or `TRUNCATE` (also inside `EXECUTE IMMEDIATE '...'`) are rejected client-side with `42000`, and `SQL_DATA_SOURCE_READ_ONLY` is `Y`. |
| `AnsiEncoding` | enum, `utf8` on Unix / `system` on Windows | Encoding of `SQL_C_CHAR` buffers and of the narrow (non-W) API strings: `utf8` (`utf-8`, `unicode`) or `system` (`ansi`, `native`, `locale`: the process code page on Windows). |

### Logging

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

| Key (aliases) | Type / default | Description |
|---|---|---|
| `LogLevel` | int 0..5, `0` | `0` off, `1` error, `2` warn, `3` info, `4` debug, `5` trace. The names `off`/`none`, `error`, `warn`/`warning`, `info`, `debug`, `trace` are accepted; numbers outside 0..5 are clamped. The environment variable `TRINO_ODBC_LOG_LEVEL` (a number) overrides every DSN. The logger is process-wide: the highest level requested by any connection wins. |
| `LogPath` | dir or file | Directory (a file `trino_odbc_<pid>.log` is created inside) or file path. Default: `%TEMP%` on Windows, `$TMPDIR` or `/tmp` elsewhere. `TRINO_ODBC_LOG_PATH` overrides; otherwise the first non-empty path of the process wins. |

## Examples per authentication type

### No authentication (development cluster, plain HTTP)

```
Driver={Trino ODBC Driver};Host=trino-dev.example.com;Port=8080;UID=analyst;Catalog=tpch;Schema=tiny
```

DSN (`odbc.ini`):

```ini
[TrinoDev]
Driver=Trino ODBC Driver
Host=trino-dev.example.com
Port=8080
UID=analyst
Catalog=tpch
Schema=tiny
SSL=0
```

### LDAP / Active Directory (HTTP Basic over TLS)

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=8443;AuthenticationType=LDAP;UID=jdoe;PWD=s3cret;SSL=1;SSLVerification=Full;TrustedCerts=C:\certs\corp-root-ca.pem
```

`AuthenticationType=LDAP` may be omitted: a `PWD` implies it. `SSL=1` may be omitted: an
authenticated mode implies it (unless `AllowInsecurePassword=1`). See [LDAP-AD.md](LDAP-AD.md).

### JWT / bearer token

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=443;AuthenticationType=JWT;UID=jdoe;AccessToken=eyJhbGciOiJSUzI1NiJ9...
```

Rotating token from a file (re-read at every connect):

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;UID=svc-report;AccessTokenFile=/var/run/secrets/trino-token
```

### Kerberos (SPNEGO)

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=7778;AuthenticationType=Kerberos;KerberosServiceName=HTTP;KerberosDelegation=0;SSL=1;TrustedCerts=/etc/security/trino-ca.pem
```

`UID` is optional: the Kerberos principal identifies the user. Obtain a ticket first (`kinit
jdoe@CORP.EXAMPLE.COM`; on Windows the domain logon ticket is used).

### OAuth2 (external browser flow)

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=443;AuthenticationType=OAuth2;ExternalAuthTimeout=180;ExternalAuthRedirectHandler=open
```

The first request is answered by Trino with a login URL; the driver opens it in the default
browser and polls the token endpoint (about once a second) until the login completes or
`ExternalAuthTimeout` elapses. When `UID` is set, the token is cached process-wide for the
`scheme://host:port` + `UID` pair, so a second connection from the same application does not prompt
again; without `UID` the token is kept only by that connection. A cached token that the server
rejects is dropped and a new browser login starts. Use `ExternalAuthRedirectHandler=print` on
servers without a browser (the URL is written to the log at level 3). This flow has not been
tested against a real identity provider in this project.

### Client certificate (mutual TLS)

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=443;AuthenticationType=Certificate;ClientCert=/home/jdoe/.trino/client.p12;ClientKeyPassword=changeit
```

or with a PEM pair:

```
...;AuthenticationType=Certificate;ClientCert=/home/jdoe/.trino/client.pem;ClientKey=/home/jdoe/.trino/client.key
```

### Full-featured DSN

```ini
[TrinoProd]
Driver=Trino ODBC Driver
Description=Production Trino (LDAP)
Host=trino.corp.example.com
Port=8443
Catalog=hive
Schema=analytics
AuthenticationType=LDAP
UID=jdoe
SSL=1
SSLVerification=Full
TrustedCerts=/etc/ssl/certs/corp-ca.pem
MinTLSVersion=1.2
Source=powerbi-finance
ClientTags=odbc,finance
TimeZone=Europe/Istanbul
SessionProperties={query_max_run_time=2h;hive.parquet_use_column_names=true}
Roles={hive=ROLE{analyst}}
ConnectTimeout=30
RequestTimeout=120
QueryTimeout=3600
IdleFetchTimeout=600
MaxRetries=5
Spooling=1
TargetResultSize=32
PrefetchPages=6
MaxPrefetchBytes=128
MaxParallelSegments=4
UseUnicodeSqlCharacterTypes=1
DefaultStringColumnLength=65535
TimestampWithTimeZoneMode=session
IntervalMode=string
LogLevel=0
```

The password is supplied by the application (Power BI credential dialog, `SQLConnect`, or
`PWD=` in the connection string).

## SQLBrowseConnect

`SQLBrowseConnect` returns `SQL_NEED_DATA` while `Host`, a required `UID` (for `None`, `LDAP`,
`JWT`) or, for `LDAP`, the `PWD` is missing. The out string then lists the attributes that are
still unset, taken from
`Host:Host=?;UID:User ID=?;PWD:Password=?;Catalog:Catalog=?;Schema:Schema=?;SSL:SSL=?` (`SSL` is
listed until it was given explicitly; `Port` is never asked for because it has a default). Once
the required values are present it connects.
