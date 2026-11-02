// TrinoODBC.pq -- Power Query custom connector for the Trino ODBC Driver.
//
// Wraps Odbc.DataSource with the SQL capabilities of Trino so that Power BI folds filters, joins,
// aggregations and LIMIT into the SQL it sends to the cluster, and enables DirectQuery.
// Credentials never come from the options record: they are supplied by Power BI's credential
// store and passed to the driver through CredentialConnectionString.
//
// Build:   powerbi/build_mez.sh  (zips this file into TrinoODBC.mez)
// Install: powerbi/README.md
// Options: docs/CONNECTION-OPTIONS.md (every driver key is accepted in the options record)

[Version = "1.0.0"]
section TrinoODBC;

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

// Name under which the driver is registered with the Windows ODBC driver manager.
DefaultDriverName = "Trino ODBC Driver";

// X-Trino-Source reported to the cluster (visible in the Trino UI and system.runtime.queries).
DefaultSource = "PowerBI";

// X-Trino-User sent with the Anonymous credential when the options record has no User field.
DefaultAnonymousUser = "powerbi";

// Set to true while developing to see the ODBC metadata handlers in the Power Query SDK trace.
EnableTraceOutput = false;

// Keys that are never passed through from the options record: connection identity comes from the
// function parameters and credentials from Power BI. Port, DSN, Driver and User are read by
// TrinoODBCImpl itself (User only as the Trino user of the Anonymous and Key credentials); the
// rest, including every alias of Host/UID/PWD/AuthenticationType/AccessToken, is dropped.
// Record field names are case-sensitive in M, so only these spellings are matched.
ReservedOptionKeys = {
    "Port", "DSN", "User", "Driver", "Host", "Server", "HostName",
    "UID", "Username", "PWD", "Password",
    "AuthenticationType", "AuthMech", "Auth",
    "AccessToken", "AccessTokenFile", "JWT", "AuthToken", "Token"
};

// ---------------------------------------------------------------------------------------------
// Data source function
// ---------------------------------------------------------------------------------------------

[DataSource.Kind = "TrinoODBC", Publish = "TrinoODBC.Publish"]
shared TrinoODBC.Contents = Value.ReplaceType(TrinoODBCImpl, TrinoODBCType);

TrinoODBCType = type function (
    server as (type text meta [
        Documentation.FieldCaption = "Server",
        Documentation.FieldDescription = "Trino coordinator host, optionally with a port (host:port). An https:// or http:// prefix selects TLS on or off. Bracket IPv6 literals: [::1]:8080.",
        Documentation.SampleValues = { "trino.example.com:8443" }
    ]),
    optional catalog as (type text meta [
        Documentation.FieldCaption = "Catalog",
        Documentation.FieldDescription = "Catalog to open. Leave empty to browse every catalog of the cluster.",
        Documentation.SampleValues = { "hive" }
    ]),
    optional options as (type record meta [
        Documentation.FieldCaption = "Advanced options",
        Documentation.FieldDescription = "Record of driver connection options, for example [Schema = ""default"", SSLVerification = ""CA"", TrustedCerts = ""C:\certs\ca.pem""], or [DSN = ""TrinoProd""] to take the settings from a DSN. Any key documented in CONNECTION-OPTIONS.md is accepted; User sets the Trino user for the Anonymous and Key credentials."
    ]))
    as table meta [
        Documentation.Name = "Trino (ODBC)",
        Documentation.LongDescription = "Connects to a Trino cluster through the Trino ODBC Driver with query folding and DirectQuery support.",
        Documentation.Examples = {[
            Description = "Browse the hive catalog of a cluster that uses LDAP authentication.",
            Code = "TrinoODBC.Contents(""trino.corp.example.com:8443"", ""hive"")",
            Result = "Navigation table of the schemas in hive"
        ], [
            Description = "Open a development cluster without TLS and pin the schema.",
            Code = "TrinoODBC.Contents(""http://trino-dev:8080"", ""tpch"", [Schema = ""tiny""])",
            Result = "Navigation table of the schemas in tpch"
        ]}
    ];

TrinoODBCImpl = (server as text, optional catalog as text, optional options as record) as table =>
    let
        opts = if options = null then [] else options,
        endpoint = ParseServer(server),
        explicitPort = Record.FieldOrDefault(opts, "Port", null),
        port = if explicitPort <> null then explicitPort else endpoint[Port],

        credential = Extension.CurrentCredential(),
        authKind = credential[AuthenticationKind],
        encrypt = Record.FieldOrDefault(credential, "EncryptConnection", null),
        anonymousUser = Record.FieldOrDefault(opts, "User", DefaultAnonymousUser),

        // Driver keys derived from the credential Power BI holds for this data source path.
        credentialConnectionString =
            if authKind = "UsernamePassword" then
                [ AuthenticationType = "LDAP", UID = credential[Username], PWD = credential[Password] ]
            else if authKind = "Key" then
                [ AuthenticationType = "JWT", AccessToken = credential[Key], UID = anonymousUser ]
            else if authKind = "Windows" then
                [ AuthenticationType = "Kerberos" ]
            else
                [ AuthenticationType = "None", UID = anonymousUser ],

        // Base connection string: DSN (settings from the DSN, host/port from the parameters win)
        // or the driver by name.
        dsn = Record.FieldOrDefault(opts, "DSN", null),
        driverName = Record.FieldOrDefault(opts, "Driver", DefaultDriverName),
        identity = if dsn <> null then [ DSN = dsn ] else [ Driver = driverName ],
        base = identity & [ Host = endpoint[Host], Port = port ],

        encryptSetting = if encrypt = true then [ SSL = 1 ] else [],
        schemeSetting =
            if endpoint[Scheme] = "https" then [ SSL = 1 ]
            else if endpoint[Scheme] = "http" then [ SSL = 0 ]
            else [],
        catalogSetting = if catalog <> null and catalog <> "" then [ Catalog = catalog ] else [],
        passthrough = Record.RemoveFields(opts, ReservedOptionKeys, MissingField.Ignore),

        // Later records win: explicit options beat the scheme, which beats the Encrypt checkbox;
        // the catalog parameter always wins.
        connectionRecord = RemoveNulls(Record.Combine({
            [ Source = DefaultSource ],
            base,
            encryptSetting,
            schemeSetting,
            passthrough,
            catalogSetting
        })),
        connectionString = ToConnectionValues(connectionRecord),

        odbcOptions = [
            HierarchicalNavigation = true,
            ClientConnectionPooling = true,
            TolerateConcatOverflow = true,
            SoftNumbers = true,
            HideNativeQuery = true,
            CredentialConnectionString = credentialConnectionString,
            SqlCapabilities = TrinoSqlCapabilities,
            SQLGetInfo = TrinoSQLGetInfo,
            SQLGetTypeInfo = TrinoSQLGetTypeInfo,
            SQLColumns = TrinoSQLColumns,
            OnError = TrinoOnError
        ],

        source = Odbc.DataSource(connectionString, odbcOptions),
        // With HierarchicalNavigation the top level lists catalogs (Kind = "Database").
        result =
            if catalog = null or catalog = "" then source
            else source{[Name = catalog, Kind = "Database"]}[Data]
    in
        result;

// ---------------------------------------------------------------------------------------------
// Data source kind and publishing
// ---------------------------------------------------------------------------------------------

TrinoODBC = [
    Label = "Trino (ODBC)",
    SupportsEncryption = true,
    Authentication = [
        UsernamePassword = [
            UsernameLabel = "User name (LDAP / Active Directory)",
            PasswordLabel = "Password",
            Label = "LDAP / password"
        ],
        Key = [
            KeyLabel = "Access token (JWT)",
            Label = "Access token"
        ],
        Windows = [],
        Anonymous = []
    ],
    // Required for the on-premises data gateway. The path is the raw server text when only the
    // required parameter was given, or a JSON record when Power BI serialises several values.
    TestConnection = (dataSourcePath) =>
        let
            parsed = try Json.Document(dataSourcePath) otherwise null,
            server = if parsed is record then Record.FieldOrDefault(parsed, "server", dataSourcePath) else dataSourcePath
        in
            { "TrinoODBC.Contents", server }
];

TrinoODBC.Publish = [
    Beta = true,
    Category = "Database",
    ButtonText = { "Trino (ODBC)", "Connect to Trino through the Trino ODBC Driver" },
    LearnMoreUrl = "https://trino.io/",
    SupportsDirectQuery = true
];

// ---------------------------------------------------------------------------------------------
// SQL capabilities
// ---------------------------------------------------------------------------------------------

// What Power Query may generate for Trino. Numeric values are the ODBC constants from sqlext.h.
TrinoSqlCapabilities = [
    SupportsTop = false,                        // Trino has no TOP ...
    LimitClauseKind = LimitClauseKind.Limit,    // ... but supports LIMIT n (Trino wants OFFSET before LIMIT, so not LimitOffset)
    // SQL_SC_SQL92_FULL. Deliberately higher than the driver's SQL_SQL_CONFORMANCE answer
    // (SQL_SC_SQL92_ENTRY): Power Query folds more with it, and the constructs it generates (joins,
    // CASE, CAST, derived tables, subqueries) are all supported by Trino.
    Sql92Conformance = 8,
    // SQL_GB_GROUP_BY_CONTAINS_SELECT, equal to the driver's SQL_GROUP_BY answer: every
    // non-aggregated select column must be grouped; GROUP BY may contain additional columns.
    // (4 would be SQL_GB_COLLATE, which Trino does not support.)
    GroupByCapabilities = 2,
    FractionalSecondsScale = 3,                 // Trino's default timestamp(3)
    SupportsNumericLiterals = true,
    SupportsStringLiterals = true,
    SupportsOdbcDateLiterals = true,            // {d '...'}  -> DATE '...'
    SupportsOdbcTimeLiterals = true,            // {t '...'}  -> TIME '...'
    SupportsOdbcTimestampLiterals = true,       // {ts '...'} -> TIMESTAMP '...'
    StringLiteralEscapeCharacters = { "'" },
    SupportsDerivedTable = true,
    PrepareStatements = false
];

// SQLGetInfo answers Power Query should rely on. They equal what the driver reports
// (src/odbc/api_info.cpp); overriding them here makes folding independent of the driver manager
// and driver build in use.
TrinoSQLGetInfo = [
    // SQL_SP_EXISTS | ISNOTNULL | ISNULL | LIKE | IN | BETWEEN | COMPARISON | QUANTIFIED_COMPARISON
    SQL_SQL92_PREDICATES = 1 + 2 + 4 + 512 + 1024 + 2048 + 4096 + 8192,
    // SQL_AF_AVG | COUNT | MAX | MIN | SUM | DISTINCT | ALL
    SQL_AGGREGATE_FUNCTIONS = 1 + 2 + 4 + 8 + 16 + 32 + 64,
    // SQL_SRJO_CROSS_JOIN | EXCEPT_JOIN | FULL_OUTER_JOIN | INNER_JOIN | INTERSECT_JOIN | LEFT_OUTER_JOIN
    // | NATURAL_JOIN | RIGHT_OUTER_JOIN | UNION_JOIN
    SQL_SQL92_RELATIONAL_JOIN_OPERATORS = 2 + 4 + 8 + 16 + 32 + 64 + 256,  // no NATURAL JOIN (128) or UNION JOIN (512) in Trino
    // SQL_SVE_CASE | CAST | COALESCE | NULLIF
    SQL_SQL92_VALUE_EXPRESSIONS = 1 + 2 + 4 + 8,
    // SQL_SQ_COMPARISON | EXISTS | IN | QUANTIFIED | CORRELATED_SUBQUERIES
    SQL_SUBQUERIES = 1 + 2 + 4 + 8 + 16,
    // SQL_U_UNION | SQL_U_UNION_ALL
    SQL_UNION = 1 + 2
];

// SQLGetTypeInfo is passed through unchanged; the hook only exists for tracing during development.
TrinoSQLGetTypeInfo = (types as table) as table =>
    if EnableTraceOutput <> true then types
    else Diagnostics.Trace(TraceLevel.Information, "TrinoODBC: SQLGetTypeInfo", () => types, true);

// SQLColumns fix-up: the driver reports json, array/map/row (and unbounded varchar when
// UnboundedVarcharAsLongVarchar=1) as SQL_(W)LONGVARCHAR. Power Query treats the long types
// conservatively, so present them as ordinary (W)VARCHAR; the values are plain text either way.
TrinoSQLColumns = (catalogName, schemaName, tableName, columnName, source) =>
    let
        fixType = (t) => if t = -10 then -9 else if t = -1 then 12 else t,   // SQL_WLONGVARCHAR -> SQL_WVARCHAR, SQL_LONGVARCHAR -> SQL_VARCHAR
        fixed = Table.TransformColumns(source, {
            { "DATA_TYPE", fixType, Int16.Type },
            { "SQL_DATA_TYPE", fixType, Int16.Type }
        }, null, MissingField.Ignore),
        traced =
            if EnableTraceOutput <> true then fixed
            else Diagnostics.Trace(TraceLevel.Information, "TrinoODBC: SQLColumns " & Text.From(tableName), () => fixed, true)
    in
        traced;

// Friendlier messages for the two errors users hit most; everything else is passed through with
// the driver's [Trino][ODBC Driver] text and SQLSTATE intact.
TrinoOnError = (errorRecord as record) =>
    let
        detail = Record.FieldOrDefault(errorRecord, "Detail", null),
        odbcErrors = if detail is record then Record.FieldOrDefault(detail, "OdbcErrors", null) else null,
        sqlStates = if odbcErrors is table then Table.Column(odbcErrors, "SQLState") else {},
        hasState = (state as text) as logical => List.Contains(sqlStates, state)
    in
        if hasState("IM002") then
            error Error.Record("DataSource.Error",
                "The Trino ODBC Driver is not installed for 64-bit applications, or the DSN does not exist (SQLSTATE IM002). Install the 64-bit driver and use a System DSN.",
                errorRecord)
        else if hasState("28000") then
            error Error.Record("DataSource.Error",
                "Trino rejected the credentials (SQLSTATE 28000): " & Record.FieldOrDefault(errorRecord, "Message", "")
                & " Check the credential type (user name/password = LDAP, Windows = Kerberos, Key = JWT) and that the server uses TLS.",
                errorRecord)
        else
            error errorRecord;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

// "https://host:8443/ui" -> [Host = "host", Port = 8443, Scheme = "https"]; "[::1]:8080" -> IPv6.
ParseServer = (server as text) as record =>
    let
        trimmed = Text.Trim(server),
        lower = Text.Lower(trimmed),
        scheme =
            if Text.StartsWith(lower, "https://") then "https"
            else if Text.StartsWith(lower, "http://") then "http"
            else null,
        withoutScheme =
            if scheme = "https" then Text.Range(trimmed, 8)
            else if scheme = "http" then Text.Range(trimmed, 7)
            else trimmed,
        slash = Text.PositionOf(withoutScheme, "/"),
        authority = if slash >= 0 then Text.Range(withoutScheme, 0, slash) else withoutScheme,
        bracketed = Text.StartsWith(authority, "["),
        closing = if bracketed then Text.PositionOf(authority, "]") else -1,
        lastColon = Text.PositionOf(authority, ":", Occurrence.Last),
        host =
            if bracketed and closing > 0 then Text.Range(authority, 1, closing - 1)
            else if lastColon >= 0 then Text.Range(authority, 0, lastColon)
            else authority,
        portText =
            if bracketed then
                (if closing >= 0 and Text.Length(authority) > closing + 1 and Text.Middle(authority, closing + 1, 1) = ":"
                 then Text.Range(authority, closing + 2) else null)
            else if lastColon >= 0 then Text.Range(authority, lastColon + 1)
            else null,
        port = if portText = null or portText = "" then null else Number.FromText(portText)
    in
        [ Host = host, Port = port, Scheme = scheme ];

RemoveNulls = (r as record) as record =>
    Record.SelectFields(r, List.Select(Record.FieldNames(r), (name) => Record.Field(r, name) <> null));

// Odbc.DataSource serialises the record into "Key=Value;..." — render every value as the text the
// driver parses (booleans as 1/0, numbers without formatting).
ToOptionText = (v as any) as text =>
    if v is logical then (if v then "1" else "0")
    else if v is number then Number.ToText(v)
    else if v is text then v
    else Text.From(v);

ToConnectionValues = (r as record) as record =>
    Record.FromList(List.Transform(Record.FieldValues(r), ToOptionText), Record.FieldNames(r));
