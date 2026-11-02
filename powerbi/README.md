# Power Query custom connector for Trino

`TrinoODBC.pq` is a Power Query (M) custom connector that wraps the Trino ODBC Driver with
Trino's SQL capabilities. Compared with Power BI's generic *ODBC* connector it adds **DirectQuery**,
better **query folding** (`LIMIT`, SQL-92 predicates, aggregates, joins, subqueries) and a
credential dialog that maps directly onto the driver's authentication types.

> **Status.** The Windows x64 driver this connector needs has been installed and used on Windows,
> including from Power BI Desktop (see `docs/INSTALL-WINDOWS.md`). The connector itself has not
> been verified separately in Power BI Desktop, the Power Query SDK or a gateway. What is tested
> automatically is the driver side: the ODBC call sequence Power BI uses for an import
> (`tests/integration/test_powerbi_flow.cpp`), against the mock server and a real Trino 483 on
> macOS and Linux.

| File | Purpose |
|---|---|
| `TrinoODBC.pq` | the connector (section document) |
| `TrinoODBC.query.pq` | test query for the Power Query SDK |
| `build_mez.sh` | packs `TrinoODBC.pq` (plus `resources.resx` / `*.png` when present) into `TrinoODBC.mez` |

## Requirements

* The 64-bit Trino ODBC Driver installed on every machine that evaluates the queries
  (Power BI Desktop PCs, on-premises data gateway machines). See `docs/INSTALL-WINDOWS.md`.
* Power BI Desktop (any recent release) and, for the Power BI service, an on-premises data
  gateway in standard mode.

## Build

```sh
powerbi/build_mez.sh                  # -> powerbi/dist/TrinoODBC.mez
powerbi/build_mez.sh /some/dir        # -> /some/dir/TrinoODBC.mez
```

The script needs bash and either `zip` or `python3`. A `.mez` is a plain ZIP archive, so on
Windows without bash:

```powershell
New-Item -ItemType Directory -Force powerbi\dist | Out-Null
Remove-Item powerbi\dist\TrinoODBC.mez -ErrorAction SilentlyContinue
Compress-Archive -Path powerbi\TrinoODBC.pq -DestinationPath powerbi\dist\TrinoODBC.zip -Force
Rename-Item powerbi\dist\TrinoODBC.zip TrinoODBC.mez
```

The Power Query SDK (Visual Studio Code extension *Power Query SDK*) can open the `powerbi/`
folder. *Set credential* stores a test credential for the data source path, and *Evaluate current
power query file* runs `TrinoODBC.query.pq`. The SDK's build task also produces a `.mez`, in its
own output folder (`bin\...`).

## Install in Power BI Desktop

1. Copy `TrinoODBC.mez` to `%USERPROFILE%\Documents\Power BI Desktop\Custom Connectors\`
   (create the folder if needed).
2. *File > Options and settings > Options > Security > Data Extensions*: select
   *(Not Recommended) Allow any extension to load without validation or warning*.
   Alternatively, sign the connector with a code-signing certificate using the Power Query SDK's
   `MakePQX` tool. `MakePQX sign` produces a signed `TrinoODBC.pqx`; copy that file instead of the
   `.mez`. Then add the certificate thumbprint to the `REG_MULTI_SZ` value
   `HKLM\SOFTWARE\Policies\Microsoft\Power BI Desktop\TrustedCertificateThumbprints`, and the
   recommended security level can stay.
3. Restart Power BI Desktop. The connector is under *Get Data > Database > Trino (ODBC) (Beta)*, or
   search for "Trino".

## Install on the on-premises data gateway

1. Install the 64-bit driver on the gateway machine.
2. In the gateway application, *Connectors* tab: enable custom connectors and set a folder,
   e.g. `C:\PowerBI\CustomConnectors`. The gateway service account (`NT SERVICE\PBIEgwService` or
   the configured domain account) needs read access to it.
3. Copy `TrinoODBC.mez` (or the signed `.pqx`) into that folder; the connector appears in the
   tab's list after a moment (restart the gateway service if it does not).
4. In the Power BI service, add a data source of type *Trino (ODBC)* to the gateway with the same
   `Server` value used in Desktop and the matching credential type. The connector's
   `TestConnection` handler calls `TrinoODBC.Contents(<server>)`.

## Using the connector

`TrinoODBC.Contents(server as text, optional catalog as text, optional options as record)`:

| Parameter | Meaning |
|---|---|
| `Server` (required) | `host`, `host:port`, `https://host:port` or `http://host:port`; IPv6 in brackets (`[::1]:8080`). A path after the authority is ignored. A scheme sets `SSL` explicitly (`https` = 1, `http` = 0). Otherwise the *Encrypt connection* option or the driver's defaults apply (TLS for port 443 and for every authenticated credential; port 443 with TLS, 8080 without). |
| `Catalog` (optional) | Open this catalog directly instead of listing all catalogs. |
| `options` (optional record) | Any driver key from `docs/CONNECTION-OPTIONS.md`, for example `[Schema = "default", SSLVerification = "CA", TrustedCerts = "C:\certs\ca.pem", SessionProperties = "query_max_run_time=1h"]`. Logical values become `1`/`0`. The Get Data dialog shows only `Server` and `Catalog`, so the record is added by editing the `Source` step (formula bar or Advanced Editor). |

How the options record is used:

* `Port` overrides the port of `Server`.
* `DSN = "TrinoProd"` loads that DSN instead of naming the driver. Everything the connector sets
  still wins over the DSN, because connection-string keys take precedence over DSN keys: `Host`
  and `Port` from `Server`, `SSL` from the scheme or *Encrypt connection*, `Source` (default
  `PowerBI`), `Catalog`, and the credential keys.
* `Driver` replaces the registered driver name (default `Trino ODBC Driver`).
* `User` sets the Trino user for the *Anonymous* and *Key* credentials (default `powerbi`).
* These keys are dropped on purpose: `Host`, `Server`, `HostName`, `UID`, `Username`, `PWD`,
  `Password`, `AuthenticationType`, `AuthMech`, `Auth`, `AccessToken`, `AccessTokenFile`, `JWT`,
  `AuthToken` and `Token`. Credentials always come from Power BI's credential store. Field names
  are case-sensitive in M, so only these exact spellings are removed.

Credential types in the dialog:

| Credential | Driver setting |
|---|---|
| Username and password | `AuthenticationType=LDAP;UID=<user>;PWD=<password>` (LDAP / Active Directory; TLS is automatic) |
| Windows | `AuthenticationType=Kerberos` (the Windows logon ticket; the SPN `HTTP/<host>` must exist) |
| Key | `AuthenticationType=JWT;AccessToken=<key>;UID=<User option or "powerbi">`; set `[User = "<principal>"]` so `X-Trino-User` matches the token subject |
| Anonymous | `AuthenticationType=None;UID=<User option or "powerbi">` (development clusters without authentication) |
| *Encrypt connection* option | ticked: `SSL=1` (overridden by an `http://` prefix or an `SSL` option); unticked: the driver's defaults |

Power BI ticks *Encrypt connection* by default. For an unauthenticated plain-HTTP cluster, write the
server as `http://host:8080` or untick the option. LDAP, JWT, Kerberos and OAuth2 over plain HTTP
additionally need `[AllowInsecurePassword = true]` (development only).

The data source path (what Power BI keys credentials on) is the `Server` text. The catalog and
the options record are not part of it, so one credential covers every catalog of a cluster.

M example:

```
let
    Source = TrinoODBC.Contents("trino.corp.example.com:8443", "hive", [Schema = "sales", Source = "finance-reports"]),
    orders = Source{[Name = "sales", Kind = "Schema"]}[Data]{[Name = "orders", Kind = "Table"]}[Data],
    recent = Table.SelectRows(orders, each [orderdate] >= #date(2024, 1, 1))   // folds to a WHERE on "orderdate" with a date literal
in
    recent
```

## What the connector configures

* `SqlCapabilities`:
  * `LimitClauseKind.Limit` instead of `TOP`,
  * `Sql92Conformance = 8` (SQL-92 full; deliberately higher than the driver's
    `SQL_SQL_CONFORMANCE` = entry level, to allow more folding),
  * `GroupByCapabilities = 2` (`SQL_GB_GROUP_BY_CONTAINS_SELECT`, the driver's `SQL_GROUP_BY`
    answer),
  * fractional seconds scale 3, numeric and string literals, ODBC date/time/timestamp literals,
  * `'` as the string escape, derived tables, no prepared statements.
* `SQLGetInfo` overrides equal to the driver's answers for predicates (`SQL_SQL92_PREDICATES`),
  aggregate functions, relational join operators, value expressions, subqueries and `UNION`.
* `SQLColumns` fix-up presenting `SQL_(W)LONGVARCHAR` columns (json, array/map/row, and unbounded
  `varchar` with `UnboundedVarcharAsLongVarchar=1`) as `SQL_(W)VARCHAR`, so they behave like
  ordinary text in Power Query. `SQLGetTypeInfo` is passed through; with `EnableTraceOutput = true`
  both hooks trace to the SDK log.
* `HierarchicalNavigation` (catalog > schema > table), `ClientConnectionPooling`,
  `TolerateConcatOverflow`, `SoftNumbers`, `HideNativeQuery`, and an `OnError` handler that
  explains `IM002` (driver/DSN missing) and `28000` (credentials) errors.
* `X-Trino-Source` is set to `PowerBI` (override with `[Source = "..."]`).

## Troubleshooting

| Problem | Fix |
|---|---|
| Connector not listed in *Get Data* | Wrong folder, security setting not changed, or Power BI Desktop not restarted. |
| `IM002` error | The 64-bit driver is not installed on this machine (or the `DSN` given in the options does not exist). |
| `28000` error | Credential type does not match the cluster (e.g. *Windows* for an LDAP cluster), wrong password, or credentials over plain HTTP (`http://` prefix) without `AllowInsecurePassword`. |
| Certificate error | Add `[TrustedCerts = "C:\certs\ca.pem"]` or import the CA into the Windows store; `[SSLVerification = "CA"]` for a name mismatch. See `docs/SSL.md`. |
| Gateway does not show the connector | Connectors folder not configured or not readable by the service account; the gateway needs the `.mez` and the driver locally. |
| DirectQuery visual errors with `42000` | The generated SQL was rejected by Trino; enable the driver log (`TRINO_ODBC_LOG_LEVEL=4`) to see it and report it. |

More: `docs/POWERBI.md`, `docs/TROUBLESHOOTING.md`.
