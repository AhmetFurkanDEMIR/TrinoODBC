# Power BI

> **Status.** Power BI Desktop on Windows x64 has been fully tested by the maintainer with the
> released NSIS package (connect, navigate, preview, import, refresh, cancel): no problems found.
> The optional `TrinoODBC.mez` custom connector adds DirectQuery and tuned folding; the Power BI
> gateway has not been tested separately.

Two ways to use Trino from Power BI:

| | Generic ODBC connector (*Get Data > ODBC*) | Trino custom connector (`TrinoODBC.mez`, built from `powerbi/`) |
|---|---|---|
| Storage mode | Import only | Import **and DirectQuery** |
| Query folding | Basic (what Power BI infers from `SQLGetInfo`) | Tuned for Trino (`LIMIT`, SQL-92 predicates, aggregates, joins) |
| Configuration | DSN or connection string typed in the dialog | `Server`, `Catalog`, optional options record (M); credentials from the Power BI credential store |
| Installation | none beyond the driver | copy the `.mez` file; adjust the security setting or sign it |
| Gateway | supported | supported (connectors folder on the gateway) |

Both need the **64-bit** driver installed on every machine that runs the queries: each analyst's
PC for Power BI Desktop and the on-premises data gateway machine(s) for the Power BI service.

## 1. Install the driver and create a DSN

Follow [INSTALL-WINDOWS.md](INSTALL-WINDOWS.md). Create a **System DSN** with the 64-bit ODBC
administrator (`C:\Windows\System32\odbcad32.exe`); a system DSN is visible to the gateway
service account too. Recommended settings (the `;` comments are explanations only, not part of the
values):

```
Host=trino.corp.example.com
Port=8443
AuthenticationType=LDAP          ; or Kerberos / JWT / OAuth2 / Certificate
UID=<user>                       ; leave PWD out; Power BI supplies it
SSL=1
SSLVerification=Full
TrustedCerts=C:\certs\corp-root-ca.pem   ; or import the CA into the Windows store
Catalog=hive                     ; optional default catalog
Source=powerbi                   ; shows up in the Trino UI
ClientTags=powerbi               ; for resource groups
TimeZone=Europe/Istanbul         ; the driver default; "system" = the gateway's OS zone
RequestTimeout=300               ; per HTTP request; long first pages need more than the default 60 s
QueryTimeout=0                   ; let Power BI's Command timeout decide
IdleFetchTimeout=600             ; cancel queries of refreshes Power BI abandoned (section 7)
Spooling=1
TargetResultSize=32
MaxPrefetchBytes=128
UseUnicodeSqlCharacterTypes=1    ; default
TimestampWithTimeZoneMode=session ; default: values in the TimeZone zone; "utc" or "wallclock"
```

Tick *SSL* in the dialog for every authentication type other than None. The dialog stores `SSL=0`
when the box is unticked, and an explicit `SSL=0` disables the driver's automatic TLS for
authenticated connections. Do not tick *Save password* for shared machines, because Power BI
stores the credentials itself.

With the custom connector, keys that the connector sets itself override the DSN: `Host` and `Port`
from *Server*, `SSL` from the scheme or *Encrypt connection*, `Source` (`PowerBI` unless the options
record sets it), `Catalog`, and the credential keys (see section 3).

## 2. Generic ODBC connector (Import)

1. *Get Data > More... > Other > ODBC*.
2. Pick the DSN, or expand *Advanced options* and paste a connection string:
   `Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=8443;Catalog=hive;SSL=1`.
   The *SQL statement* box sends the text as-is (no folding on top of it).
3. Credentials:
   * *Database*: user name + password → sent as `UID`/`PWD` (LDAP / Active Directory).
   * *Windows*: the current Windows account; works only when the DSN says `AuthenticationType=Kerberos`
     (Power BI does not pass the Windows password to ODBC drivers).
   * *Default or Custom*: only what the DSN/connection string already contains (`AccessToken`,
     `AuthenticationType=OAuth2`, certificates, or a saved password). Extra keys can be typed in
     the *Custom* box, e.g. `AuthenticationType=JWT;AccessToken=eyJ...`.
4. The navigator shows catalogs > schemas > tables/views (from `SQLTables`); columns come from
   `SQLColumns` with the types of [TYPES.md](TYPES.md).

The `CommandTimeout` option of `Odbc.DataSource` / `Odbc.Query` reaches the driver as
`SQL_ATTR_QUERY_TIMEOUT`; the driver cancels the Trino query when it expires. `ConnectionTimeout`
reaches it as `SQL_ATTR_LOGIN_TIMEOUT`, which the driver uses as the connect timeout when it is
set before connecting. Power BI also sets it on open connections, where it is accepted and ignored.

## 3. Custom connector (Import + DirectQuery)

Build `TrinoODBC.mez` (see [../powerbi/README.md](../powerbi/README.md)), copy it to
`%USERPROFILE%\Documents\Power BI Desktop\Custom Connectors\`, and in Power BI Desktop set
*File > Options and settings > Options > Security > Data Extensions* to *Allow any extension to
load without validation or warning* (or sign the connector and register the certificate thumbprint
under `HKLM\SOFTWARE\Policies\Microsoft\Power BI Desktop\TrustedCertificateThumbprints`). Restart
Power BI Desktop.

*Get Data > Trino (ODBC)*:

| Field | Meaning |
|---|---|
| Server | `host` or `host:port`; `https://host:8443` / `http://host:8080` set `SSL=1` / `SSL=0` explicitly |
| Catalog | optional; opens that catalog directly |
| options (M only) | optional record of driver keys, e.g. `[Schema = "default", SSLVerification = "CA", TrustedCerts = "C:\certs\ca.pem", Source = "finance"]`, or `[DSN = "TrinoProd"]` to read the remaining settings from a DSN. The dialog shows only *Server* and *Catalog*, so add the record by editing the `Source` step: `TrinoODBC.Contents("trino.corp.example.com:8443", "hive", [Schema = "default"])`. |

Credential types: *Username and password* (LDAP/AD), *Windows* (Kerberos), *Key* (a JWT access
token; add `[User = "<principal>"]` to the options so `X-Trino-User` matches the token's
subject, otherwise the user is `powerbi`), *Anonymous* (no authentication; `[User = "<name>"]` sets
the Trino user, default `powerbi`). A ticked *Encrypt connection* (Power BI's default) sends
`SSL=1` unless the server has an `http://` prefix or the options set `SSL`. For an
unauthenticated plain-HTTP cluster, use `http://host:8080` or untick it. The details are in
[../powerbi/README.md](../powerbi/README.md#using-the-connector).

The connector wraps `Odbc.DataSource` with Trino's SQL capabilities:

* `LimitClauseKind = Limit` — `Table.FirstN` / previews become `... LIMIT n` (Trino has no `TOP`).
* `Sql92Conformance = 8` (SQL-92 full). This is deliberately higher than the driver's
  `SQL_SQL_CONFORMANCE` (entry level) so that more folds.
* `GroupByCapabilities = 2` (`SQL_GB_GROUP_BY_CONTAINS_SELECT`, as the driver reports): every
  non-aggregated select column is grouped, and extra grouping columns are allowed.
* String, numeric and ODBC date/time/timestamp literals, fractional seconds to 3 digits, `'` as the
  string escape, derived tables.
* `SQLGetInfo` overrides that mirror the driver's own answers (predicates, aggregates, join
  operators, value expressions, subqueries, `UNION`), so folding does not depend on the
  driver-manager version.
* `SQLColumns` fix-up: long character columns (`json`, array/map/row, `varchar` with
  `UnboundedVarcharAsLongVarchar=1`) are presented as regular `varchar` so Power Query treats them
  as ordinary text.
* `HierarchicalNavigation` (catalog > schema > table), `TolerateConcatOverflow`, `SoftNumbers`,
  `HideNativeQuery`, client connection pooling.

### DirectQuery notes

* Every visual sends one or more queries; keep the coordinator close to the gateway and set
  `QueryTimeout` / the report's *Command timeout* to what a user tolerates.
* Check that a step folds: steps that do not fold (custom M functions, some text transformations)
  force Power BI to pull the whole table. The connector sets `HideNativeQuery`, so *View Native
  Query* may not be available. The driver log (`LogLevel=4`) and Trino's query list show the SQL
  that was actually sent.
* Relationships across catalogs work: Trino joins across connectors in one query.
* `SELECT` with a `LIMIT` is generated for previews; the driver adds nothing else.

## 4. Query folding notes (both connectors)

* Trino identifiers are lower-case; the driver reports `SQL_IDENTIFIER_CASE = SQL_IC_LOWER`,
  `SQL_QUOTED_IDENTIFIER_CASE = SQL_IC_SENSITIVE` and `"` as the quote character, and Power BI
  quotes every identifier it generates.
* Filters on `date`, `timestamp` and text fold through ODBC literals (`{d '...'}`, `{ts '...'}`),
  which the driver translates to `DATE '...'` / `TIMESTAMP '...'`.
* `timestamp with time zone` columns are delivered as `datetime` in the session time zone
  (default `session` mode, default `TimeZone=Europe/Istanbul`, i.e. Turkey time, UTC+03:00). This
  is a deliberate project default: set `TimeZone=system` for the gateway's OS zone or another IANA
  name, `TimestampWithTimeZoneMode=utc` for UTC `datetime` values, `wallclock` if reports must show
  the source's local time, or `string` to keep the zone.
* `decimal` columns arrive as *Decimal Number* through `SQL_NUMERIC_STRUCT`; `DecimalAsDouble=1`
  is only needed for tools that cannot handle it, not for Power BI.
* Unbounded `varchar` is reported as `varchar(65535)` (`DefaultStringColumnLength`); Power BI
  does not allocate by that size, so the default is fine.
* `array`, `map`, `row` and `json` arrive as JSON text; use `Json.Document` in Power Query to
  expand them (this does not fold).

## 5. On-premises data gateway

1. Install the 64-bit driver on the gateway machine and create the DSN as a **System DSN**
   (the gateway runs as `NT SERVICE\PBIEgwService` or a domain service account and cannot see
   user DSNs of other accounts).
2. For the custom connector: copy `TrinoODBC.mez` into the folder configured on the gateway's
   *Connectors* tab and make sure the service account has read access; the gateway lists the
   connector after a restart.
3. In the Power BI service create the data source on the gateway with the same DSN name /
   connection string (generic ODBC) or *Server* value (custom connector) as in Desktop, and the
   credential type used there (*Basic* = user name + password for LDAP, *Windows* for Kerberos).
   With the generic ODBC connector, *Anonymous* works with a DSN that carries a token or a client
   certificate. The custom connector's *Anonymous* credential sends `AuthenticationType=None`,
   which overrides the DSN; use its *Key* credential for JWT.
4. Kerberos: run the gateway service under a domain account, give that account a valid logon
   (the gateway then holds the TGT), make sure the SPN `HTTP/trino.corp.example.com` exists, and
   set `AuthenticationType=Kerberos` in the system DSN. Every query then runs as the service
   account. Single sign-on (running each query as the report viewer) is not implemented by
   either connector path. `KerberosDelegation=1` only forwards the service account's own
   credentials to Trino.
5. Network: the gateway needs access to the coordinator port and, with `Spooling=1`, to the
   spooling storage endpoint the coordinator hands out (object store URL). If that endpoint is not
   reachable from the gateway, set `Spooling=0` in the gateway DSN.
6. Logging on the gateway: set the machine-wide environment variables
   `TRINO_ODBC_LOG_LEVEL=4` and `TRINO_ODBC_LOG_PATH=C:\gateway-logs` and restart the service.

## 6. Large models

* Import: raise `TargetResultSize` (32-64 MB) and `MaxPrefetchBytes` (128+ MB), keep `Spooling=1`,
  and set `RequestTimeout` high (or 0) so a slow first page does not fail the refresh. Filter
  early in Power Query so the filter folds into the Trino query.
* Incremental refresh works with `RangeStart`/`RangeEnd` parameters on a `timestamp` or `date`
  column; the comparisons fold to `TIMESTAMP '...'` literals.
* Several datasets refreshing at once on one gateway multiply the memory per connection
  (`MaxPrefetchBytes` plus `MaxParallelSegments x TargetResultSize` of segments in flight).
* `trino_odbc_bench` ([INSTALL-LINUX.md](INSTALL-LINUX.md#benchmarking)) binds columns the way
  Power BI does (text as `SQL_C_WCHAR`). Use it to measure what a DSN's settings deliver before
  tuning a refresh.
* See [PERFORMANCE.md](PERFORMANCE.md).

## 7. Cancelling a refresh or a preview

What the driver does when Power BI stops a query:

* **Cancel button / closed preview / superseded DirectQuery visual.** Power BI (the
  `Microsoft.Mashup` evaluation container) calls `SQLCancel` from another thread, usually *between*
  two `SQLFetch` calls, then frees the statement and the connection. The driver cancels the query
  on the coordinator right away: it sends `DELETE <nextUri>` (the protocol's cancel request; the
  query ends as `FAILED` / `USER_CANCELED` in the Trino UI) and the next fetch on that cursor
  returns `HY008`. The same happens when the statement is closed or freed, the connection is
  disconnected or freed, or the statement is re-executed while rows are still streaming. The
  `DELETE` runs on a helper thread (5 s budget, one retry on a network error); `SQLDisconnect`
  waits up to 3 s for pending `DELETE`s so an evaluation container that exits right afterwards
  still reaches the coordinator.
* **Evaluation container killed.** When Power BI terminates the container process (refresh
  timeout, Desktop closed, the gateway recycling its mashup workers), the driver gets no call at
  all and cannot notify Trino. The coordinator then ends the query itself once no client has asked
  for results for `query.client.timeout` (default **5 minutes**). Administrators can lower it in
  the coordinator's `config.properties`, e.g. `query.client.timeout=2m`. Keep it above the longest
  pause a healthy client makes between two result requests: while its prefetch queue is full the
  driver does not poll the coordinator, so an application that stops reading for longer than the
  timeout sees its query fail as abandoned (`ABANDONED_QUERY`).
* **Power BI keeps the handles open but stops reading.** Some mashup code paths neither cancel
  nor close. `IdleFetchTimeout=600` (connection option, off by default) makes the driver cancel a
  query that is still running on the server when no ODBC call was made on its statement for 600 s;
  the cursor then reports `HY008`. Results the server has already delivered completely are never
  affected, and a slow but active reader is not cancelled (every `SQLFetch` / `SQLGetData` counts
  as activity). It matters most for long-running queries that have not produced their rows yet:
  the driver keeps polling those, which keeps them alive on the coordinator, whereas a query whose
  prefetch queue is full is no longer polled and is ended by `query.client.timeout` anyway.

To confirm a cancel reached Trino, look the query up in the Trino UI (or
`GET /v1/query/<queryId>` with an `X-Trino-User` header): state `FAILED`, error `USER_CANCELED`.
With `LogLevel=3` or higher the driver log shows `cancelling query <id>` (or `closing unfinished
query <id>`) followed by `DELETE ... -> HTTP 204` at `LogLevel=4`.

## 8. Troubleshooting

| Message | Cause / fix |
|---|---|
| `DataSource.Error: ODBC: ERROR [IM002] [Microsoft][ODBC Driver Manager] Data source name not found and no default driver specified` | 64-bit driver not installed / DSN created with the 32-bit administrator / user DSN of another account. Use a System DSN. |
| `DataSource.Error: ODBC: ERROR [HY000] [Trino][ODBC Driver] Host is required` | The DSN or connection string has no `Host=`. |
| `DataSource.Error: ODBC: ERROR [28000] [Trino][ODBC Driver] Password authentication requires SSL` | An explicit `SSL=0` with a password: SSL unticked in the DSN dialog, `SSL=0` in the connection string, or an `http://` server in the custom connector. Enable SSL and use the HTTPS port, or set `AllowInsecurePassword=1` on a development cluster. |
| `DataSource.Error: ODBC: ERROR [28000] [Trino][ODBC Driver] HTTP 401` | Wrong credentials, or the credential type does not match `AuthenticationType` (e.g. *Windows* chosen for an LDAP DSN). |
| `DataSource.Error: ODBC: ERROR [08001] ... certificate` | The gateway/PC does not trust the Trino certificate: `TrustedCerts` or import the CA into the machine store ([SSL.md](SSL.md)). |
| `DataSource.Error: ODBC: ERROR [HYT00] [Trino][ODBC Driver] ... timed out` | `RequestTimeout` (per page) or the *Command timeout*; raise both. |
| `DataSource.Error: ODBC: ERROR [HY008] ...` | Power BI cancelled the query (preview abandoned, refresh stopped), or `IdleFetchTimeout` cancelled a result nobody read for that long (the message then names `IdleFetchTimeout`). See section 7. |
| Queries keep running in the Trino UI after a cancelled refresh | The evaluation container was killed before it could cancel; Trino ends the query after `query.client.timeout` (default 5 min). Lower that setting on the coordinator and set `IdleFetchTimeout=600` in the DSN (section 7). |
| `DataSource.Error: ODBC: ERROR [42000] [Trino][ODBC Driver][Trino] line 1:...: mismatched input` | SQL Trino cannot parse: a native query typed in the *SQL statement* box, or a folded expression Trino rejects. Check the driver log (`LogLevel=4`) for the exact SQL. |
| `We couldn't authenticate with the credentials provided` | Same as `28000`; also check that the DSN does not carry a stale `PWD`. |
| Refresh in the service: `The data source is not available` / `Unable to connect` | Gateway machine lacks the driver, the DSN, network access or the CA; check the gateway logs and the driver log on the gateway. |
| DirectQuery greyed out | The generic ODBC connector supports Import only; install the custom connector. |
| Custom connector missing from *Get Data* | Wrong folder, security option not set, Power BI not restarted; in the gateway the connectors folder is not configured or unreadable. |
| Slow navigator with many schemas | The navigator issues one `SQLTables` per level; open a specific catalog (custom connector `Catalog` field, or `Catalog=` in the DSN) rather than the whole cluster. |

More in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
