# ODBC capabilities

What the driver reports through `SQLGetInfo` / `SQLGetFunctions` and what it actually implements.
The values mirror `src/odbc/api_info.cpp`; they are chosen to describe Trino truthfully, so an
application that follows them generates SQL Trino accepts.

## Conformance and identification

| Info type | Value |
|---|---|
| `SQL_DRIVER_NAME` | `trinoodbc.dll` / `libtrinoodbc.so` / `libtrinoodbc.dylib` |
| `SQL_DRIVER_VER` | `01.00.0000` |
| `SQL_DRIVER_ODBC_VER` | `03.80` |
| `SQL_ODBC_VER` (when asked directly, without a driver manager) | `03.80.0000` |
| `SQL_DBMS_NAME` | `Trino` |
| `SQL_DBMS_VER` | `483.00.0000 Trino 483` (up to three numeric components of the Trino version formatted `%02d.%02d.%04d`, followed by ` Trino ` and the raw version); `00.00.0000` when unknown (`ValidateConnection=0`) |
| `SQL_SERVER_NAME` | the `Host` |
| `SQL_DATA_SOURCE_NAME` | the DSN used (empty for a DSN-less connection) |
| `SQL_USER_NAME` | `UID` |
| `SQL_DATABASE_NAME` | current catalog (`SQL_ATTR_CURRENT_CATALOG`) |
| `SQL_ODBC_INTERFACE_CONFORMANCE` | `SQL_OIC_CORE` |
| `SQL_ODBC_API_CONFORMANCE` | `SQL_OAC_LEVEL1` |
| `SQL_ODBC_SQL_CONFORMANCE` | `SQL_OSC_CORE` |
| `SQL_SQL_CONFORMANCE` | `SQL_SC_SQL92_ENTRY` |
| `SQL_STANDARD_CLI_CONFORMANCE` | `SQL_SCC_ISO92_CLI` |
| `SQL_ODBC_SAG_CLI_CONFORMANCE` | `SQL_OSCC_COMPLIANT` |
| `SQL_XOPEN_CLI_YEAR` | `1995` |
| `SQL_ASYNC_MODE` / `SQL_ASYNC_DBC_FUNCTIONS` / `SQL_ASYNC_NOTIFICATION` | none / not capable |
| `SQL_DRIVER_AWARE_POOLING_SUPPORTED` | not capable |
| `SQL_ACTIVE_ENVIRONMENTS`, `SQL_MAX_CONCURRENT_ACTIVITIES`, `SQL_MAX_DRIVER_CONNECTIONS`, `SQL_MAX_ASYNC_CONCURRENT_STATEMENTS` | 0 (no limit) |
| `SQL_FILE_USAGE` | `SQL_FILE_NOT_SUPPORTED` |
| `SQL_DRIVER_HDBC`, `SQL_DRIVER_HENV`, `SQL_DRIVER_HSTMT`, `SQL_DRIVER_HDESC`, `SQL_DRIVER_HLIB`, `SQL_DM_VER` | answered by the driver manager; asked directly they return `HY096` |

## SQL grammar and identifiers

| Info type | Value |
|---|---|
| `SQL_IDENTIFIER_QUOTE_CHAR` | `"` |
| `SQL_IDENTIFIER_CASE` | `SQL_IC_LOWER` (unquoted identifiers are lower-cased by Trino) |
| `SQL_QUOTED_IDENTIFIER_CASE` | `SQL_IC_SENSITIVE` |
| `SQL_SPECIAL_CHARACTERS` | (empty) |
| `SQL_SEARCH_PATTERN_ESCAPE` | `\` |
| `SQL_LIKE_ESCAPE_CLAUSE` | `Y` |
| `SQL_CATALOG_NAME` | `Y`; term `catalog`; separator `.`; `SQL_CATALOG_LOCATION` = `SQL_CL_START` |
| `SQL_CATALOG_USAGE` / `SQL_SCHEMA_USAGE` | DML statements, table definition, index definition, privilege definition |
| `SQL_SCHEMA_TERM` / `SQL_TABLE_TERM` / `SQL_PROCEDURE_TERM` | `schema` / `table` / `procedure` |
| `SQL_KEYWORDS` | the complete list of Trino reserved words, SQL-92 ones included (`ALTER, AND, AS, BETWEEN, BY, CASE, CAST, CONSTRAINT, CREATE, CROSS, CUBE, CURRENT_CATALOG, ..., JSON_TABLE, JSON_VALUE, LISTAGG, LOCALTIME, LOCALTIMESTAMP, NATURAL, NORMALIZE, ..., UESCAPE, UNION, UNNEST, USING, VALUES, WHEN, WHERE, WITH`; 82 words) |
| `SQL_COLUMN_ALIAS` | `Y` |
| `SQL_CORRELATION_NAME` | `SQL_CN_ANY` |
| `SQL_EXPRESSIONS_IN_ORDERBY` | `Y` |
| `SQL_ORDER_BY_COLUMNS_IN_SELECT` | `N` |
| `SQL_GROUP_BY` | `SQL_GB_GROUP_BY_CONTAINS_SELECT` |
| `SQL_OUTER_JOINS` | `Y`; `SQL_OJ_CAPABILITIES` = left, right, full, nested, not ordered, inner, all comparison operators |
| `SQL_SQL92_RELATIONAL_JOIN_OPERATORS` | cross, except, full outer, inner, intersect, left outer, right outer (no natural join and no union join: Trino rejects both) |
| `SQL_SQL92_PREDICATES` | between, comparison, exists, in, is [not] null, like, quantified comparison |
| `SQL_SQL92_VALUE_EXPRESSIONS` | case, cast, coalesce, nullif |
| `SQL_SQL92_NUMERIC_VALUE_FUNCTIONS` | extract, position (Trino has no native `char_length`/`character_length`/`octet_length`; the `{fn ...}` escapes for them are translated and reported through `SQL_STRING_FUNCTIONS`) |
| `SQL_SQL92_STRING_FUNCTIONS` | lower, upper, substring, trim both/leading/trailing |
| `SQL_SQL92_DATETIME_FUNCTIONS` | current_date, current_time, current_timestamp |
| `SQL_SQL92_ROW_VALUE_CONSTRUCTOR` | value expression, null, row subquery |
| `SQL_SQL92_GRANT` / `SQL_SQL92_REVOKE` / foreign-key rules | 0 |
| `SQL_AGGREGATE_FUNCTIONS` | all, avg, count, distinct, max, min, sum |
| `SQL_SUBQUERIES` | correlated, comparison, exists, in, quantified |
| `SQL_UNION` | union, union all |
| `SQL_DATETIME_LITERALS` | date, time, timestamp and every interval literal |
| `SQL_INSERT_STATEMENT` | literals, searched |
| `SQL_CREATE_TABLE` / `SQL_CREATE_VIEW` / `SQL_CREATE_SCHEMA` | create table / create view / create schema |
| `SQL_DROP_TABLE` / `SQL_DROP_VIEW` / `SQL_DROP_SCHEMA` | drop table / drop view / drop schema |
| `SQL_ALTER_TABLE` | add column, drop column |
| `SQL_DDL_INDEX`, `SQL_CREATE_*`/`SQL_DROP_*` for assertions, character sets, collations, domains, translations | 0 |
| `SQL_INDEX_KEYWORDS` | none |
| `SQL_INFO_SCHEMA_VIEWS` | columns, schemata, tables, views |
| `SQL_INTEGRITY` | `N` |
| `SQL_PROCEDURES` / `SQL_ACCESSIBLE_PROCEDURES` | `N` |
| `SQL_ACCESSIBLE_TABLES` | `Y` |
| `SQL_CONCAT_NULL_BEHAVIOR` | `SQL_CB_NULL` |
| `SQL_NULL_COLLATION` | `SQL_NC_END` (Trino sorts NULLs last for both ASC and DESC) |
| `SQL_NON_NULLABLE_COLUMNS` | `SQL_NNC_NON_NULL` |
| `SQL_COLLATION_SEQ` | (empty) |

## Limits

`SQL_MAX_IDENTIFIER_LEN`, `SQL_MAX_CATALOG_NAME_LEN`, `SQL_MAX_SCHEMA_NAME_LEN`,
`SQL_MAX_TABLE_NAME_LEN`, `SQL_MAX_COLUMN_NAME_LEN`, `SQL_MAX_CURSOR_NAME_LEN`,
`SQL_MAX_PROCEDURE_NAME_LEN`, `SQL_MAX_USER_NAME_LEN` = 128. Every other `SQL_MAX_*` (columns in
select/group by/order by/table/index, tables in select, row size, statement length, literal
lengths, index size) = 0 (no limit / unknown). `SQL_MAX_ROW_SIZE_INCLUDES_LONG` = `Y`.

## Cursors and fetching

| Info type | Value |
|---|---|
| `SQL_SCROLL_OPTIONS` | `SQL_SO_FORWARD_ONLY` |
| `SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1` | `SQL_CA1_NEXT`, `SQL_CA1_LOCK_NO_CHANGE`, `SQL_CA1_POS_POSITION` |
| `SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2` | `SQL_CA2_READ_ONLY_CONCURRENCY`, `SQL_CA2_MAX_ROWS_SELECT` |
| static / keyset / dynamic attributes | 0 |
| `SQL_CURSOR_SENSITIVITY` | unspecified |
| `SQL_CURSOR_COMMIT_BEHAVIOR` / `SQL_CURSOR_ROLLBACK_BEHAVIOR` | `SQL_CB_PRESERVE` |
| `SQL_BOOKMARK_PERSISTENCE` | 0 |
| `SQL_GETDATA_EXTENSIONS` | `SQL_GD_ANY_COLUMN`, `SQL_GD_ANY_ORDER`, `SQL_GD_BLOCK`, `SQL_GD_BOUND` |
| `SQL_ROW_UPDATES` | `N` |
| `SQL_MULT_RESULT_SETS` | `N` |
| `SQL_NEED_LONG_DATA_LEN` | `N` |
| `SQL_FETCH_DIRECTION` (ODBC 2) | next only; `SQL_SCROLL_CONCURRENCY` read-only; `SQL_LOCK_TYPES` no change; `SQL_POS_OPERATIONS` position; `SQL_POSITIONED_STATEMENTS` 0; `SQL_STATIC_SENSITIVITY` 0 |

What that means in practice:

* Forward-only, read-only cursors. `SQLFetchScroll` accepts `SQL_FETCH_NEXT` only (`HY106`
  otherwise); `SQLExtendedFetch` is supported with the same restriction.
* Block cursors of any size: `SQL_ATTR_ROW_ARRAY_SIZE` (and ODBC 2 `SQL_ROWSET_SIZE`),
  `SQL_ATTR_ROW_BIND_TYPE` (column-wise or row-wise), `SQL_ATTR_ROW_BIND_OFFSET_PTR`,
  `SQL_ATTR_ROW_STATUS_PTR`, `SQL_ATTR_ROWS_FETCHED_PTR`, `SQL_ATTR_ROW_OPERATION_PTR`.
* `SQLGetData` on any column in any order, also for bound columns, in chunks (`01004` for each
  partial chunk, then `SQL_NO_DATA` once the column is consumed). A NULL cell returns
  `SQL_SUCCESS` with `SQL_NULL_DATA` once, then `SQL_NO_DATA`. `SQL_C_CHAR` chunks are cut at byte
  boundaries (a chunk smaller than one UTF-8 code point splits the sequence); a `SQL_C_WCHAR`
  buffer that cannot hold a single character returns `HY090`. `SQLSetPos(SQL_POSITION)` selects
  the row within the rowset for `SQLGetData`; other `SQLSetPos` operations and
  `SQLBulkOperations` return `HYC00`.
* `SQL_ATTR_MAX_ROWS` stops the fetch (and cancels the query) after N rows.
  `SQL_ATTR_MAX_LENGTH` caps character/binary data. `SQL_ATTR_RETRIEVE_DATA` is honoured.
* Requests for scrollable, keyset, dynamic or sensitive cursors, bookmarks, concurrency other
  than read-only, `SQL_ATTR_SIMULATE_CURSOR`, a non-zero `SQL_ATTR_KEYSET_SIZE` and asynchronous
  execution are downgraded with `01S02` (option value changed) rather than refused.

## Transactions

| Info type | `EnableTransactions=0` (default) | `EnableTransactions=1` |
|---|---|---|
| `SQL_TXN_CAPABLE` | `SQL_TC_NONE` | `SQL_TC_DML` |
| `SQL_TXN_ISOLATION_OPTION` | read uncommitted, read committed, repeatable read, serializable (always all four: Trino supports every level) | same |
| `SQL_DEFAULT_TXN_ISOLATION` | `SQL_TXN_READ_UNCOMMITTED` | same |
| `SQL_ATTR_AUTOCOMMIT = SQL_AUTOCOMMIT_OFF` | `HYC00` on an open connection; set before connecting it is kept and reset to on with `01S02` at connect | accepted; the next statement starts a Trino transaction (`X-Trino-Transaction-Id: NONE`, `START TRANSACTION ISOLATION LEVEL ...` when an isolation level was set), `SQLEndTran` sends `COMMIT` / `ROLLBACK` |
| `SQL_ATTR_TXN_ISOLATION` | accepted (any of the four levels) | accepted; changing it while a transaction is open is `HY011` |
| `SQL_MULTIPLE_ACTIVE_TXN` | `N` | `N` |
| `SQL_DTC_TRANSITION_COST` | 0 | 0 |

`SQLEndTran` with autocommit on is a no-op that succeeds; with autocommit off and
`EnableTransactions=0` it returns `HYC00`. Disconnecting with an open driver transaction returns
`25000`. Distributed transactions (`SQL_ATTR_ENLIST_IN_DTC`) are not
supported (`HYC00`).

## Statements and parameters

| Info type | Value |
|---|---|
| `SQL_DESCRIBE_PARAMETER` | `Y` (`SQLDescribeParam` uses `DESCRIBE INPUT`) |
| `SQL_PARAM_ARRAY_ROW_COUNTS` | `SQL_PARC_NO_BATCH` (parameter sets are executed one by one) |
| `SQL_PARAM_ARRAY_SELECTS` | `SQL_PAS_NO_SELECT` (paramset size > 1 with a result set is `HYC00`) |
| `SQL_BATCH_SUPPORT` / `SQL_BATCH_ROW_COUNT` | 0 |
| `SQL_DATA_SOURCE_READ_ONLY` | `Y` when `ReadOnly=1` or `SQL_ATTR_ACCESS_MODE` is read-only |

* `SQLPrepare` translates escapes and counts `?` markers; column metadata before execution comes
  from `DESCRIBE OUTPUT`, parameter metadata from `DESCRIBE INPUT` (both cached until re-prepare).
* Parameters: input only (`SQL_PARAM_INPUT`; output/inout are `HYC00`), rendered as typed Trino
  literals and executed through `EXECUTE IMMEDIATE ... USING` (or `EXECUTE name USING` with
  `ExplicitPrepare=1`). Data-at-execution via `SQLParamData` / `SQLPutData`. `SQL_ATTR_PARAMSET_SIZE`
  with the status, operation and processed-rows arrays. `SQLNumParams`, `SQLDescribeParam`,
  `SQLSetParam`, `SQLParamOptions` supported.
* `SQLRowCount` returns the Trino `updateCount` for DML (0 for DML without a count, -1 for other
  statements). `SQLMoreResults` returns `SQL_NO_DATA` (no multiple result sets).
* `SQLCancel` / `SQLCancelHandle` cancel a running statement from another thread; the cancelled
  call returns `HY008`.
* `SQLNativeSql` returns the escape-translated SQL. `SQL_ATTR_NOSCAN` disables translation.
* Cursor names: `SQLGetCursorName` / `SQLSetCursorName` (default `SQL_CUR<n>`), positioned
  updates are not supported.

## Descriptors and diagnostics

Real ODBC 3 descriptors: implicit ARD/APD/IRD/IPD per statement, explicit descriptors through
`SQLAllocHandle(SQL_HANDLE_DESC)`, `SQLSetStmtAttr(SQL_ATTR_APP_ROW_DESC / APP_PARAM_DESC)`,
`SQLGetDescField` / `SQLSetDescField` / `SQLGetDescRec` / `SQLSetDescRec` / `SQLCopyDesc`
(`SQL_ATTR_AUTO_IPD` = `SQL_TRUE`). Diagnostics: `SQLGetDiagRec` / `SQLGetDiagField` with the
header fields `SQL_DIAG_NUMBER`, `SQL_DIAG_RETURNCODE`, `SQL_DIAG_ROW_COUNT`,
`SQL_DIAG_CURSOR_ROW_COUNT`, `SQL_DIAG_DYNAMIC_FUNCTION(_CODE)` and all record fields; ODBC 2
`SQLError` is supported.

## Catalog functions

| Function | Source | Notes |
|---|---|---|
| `SQLTables` | `system.jdbc.tables` (`catalogs`, `schemas`, `table_types` for the `SQL_ALL_*` enumerations) | table type list `TABLE,VIEW` or `'TABLE','VIEW'`; patterns with `%`/`_`/`\` |
| `SQLColumns` | `system.jdbc.columns` | `DATA_TYPE`, `COLUMN_SIZE`, `BUFFER_LENGTH`, `DECIMAL_DIGITS`, `NUM_PREC_RADIX`, `SQL_DATA_TYPE`, `SQL_DATETIME_SUB`, `CHAR_OCTET_LENGTH` recomputed with the same mapping as `SQLDescribeCol` |
| `SQLGetTypeInfo` | static table (see [TYPES.md](TYPES.md)) | filtered by the requested type; ODBC 2 ids mapped |
| `SQLProcedures`, `SQLProcedureColumns` | `system.jdbc.procedures` / `procedure_columns` | empty on servers without these tables |
| `SQLPrimaryKeys`, `SQLForeignKeys`, `SQLStatistics`, `SQLSpecialColumns`, `SQLTablePrivileges`, `SQLColumnPrivileges` | — | empty result sets with the spec-mandated columns (Trino has no keys/indexes and does not expose grants) |

Pattern arguments are matched with `LIKE ... ESCAPE '\'`; with `SQL_ATTR_METADATA_ID=SQL_TRUE` or
`MetadataCaseSensitive=1` they are compared literally.

## Escape sequences

`{d 'x'}` -> `DATE 'x'`, `{t 'x'}` -> `TIME 'x'`, `{ts 'x'}` -> `TIMESTAMP 'x'`,
`{interval ...}` -> `INTERVAL ...`, `{oj ...}` -> the join text, `{escape 'c'}` -> `ESCAPE 'c'`,
`{call p(...)}` -> `CALL p(...)`, `{? = call ...}` -> `HYC00`, `{fn ...}` -> see below. Keywords are case-insensitive. Sequences
nest (up to 64 levels) and are left alone inside string literals, quoted identifiers and comments.
A body that is not a single string literal becomes a cast: `{d ?}` -> `CAST(? AS date)`, likewise
`{t ...}` / `{ts ...}` with `time` / `timestamp`. `{call p}` without an argument list becomes
`CALL p()`. `{escape ...}` accepts a quoted character or `?`. Malformed sequences (unbalanced
braces, empty bodies, unknown keywords, wrong argument counts, an unknown `SQL_TSI_*` unit or
`CONVERT` type) are `42000`. `SQL_ATTR_NOSCAN=SQL_NOSCAN_ON` sends the text untranslated.

`SQL_CONVERT_FUNCTIONS` = `CAST`, `CONVERT`; `{fn CONVERT(expr, SQL_type)}` becomes `CAST(expr AS
<trino type>)` with `SQL_BIGINT` -> `bigint`, binary types -> `varbinary`, `SQL_BIT` -> `boolean`,
character types -> `varchar`, date -> `date`, `SQL_DECIMAL`/`SQL_NUMERIC` -> `decimal(38,10)`,
`SQL_DOUBLE`/`SQL_FLOAT` -> `double`, `SQL_INTEGER` -> `integer`, `SQL_REAL` -> `real`,
`SQL_SMALLINT` -> `smallint`, `SQL_TINYINT` -> `tinyint`, time -> `time`, timestamp -> `timestamp`,
`SQL_GUID` -> `uuid`, `SQL_BOOLEAN` -> `boolean`, intervals -> `interval year to month` /
`interval day to second`. The type may be written as `SQL_VARCHAR`, `VARCHAR` or the numeric
ODBC code; a `(n)` / `(p,s)` suffix is carried over for character, decimal, time and timestamp
targets (`{fn CONVERT(x, SQL_DECIMAL(10,2))}` -> `CAST(x AS decimal(10,2))`), and
`decimal(38,10)` is used when a decimal target has none.

### Scalar functions (`SQL_STRING_FUNCTIONS`, `SQL_NUMERIC_FUNCTIONS`, `SQL_TIMEDATE_FUNCTIONS`, `SQL_SYSTEM_FUNCTIONS`)

Every function claimed in the bitmasks has a translation; functions not listed are passed through
unchanged (Trino may still know them) with a warning in the log and, for `SQLNativeSql`, a `01000`
diagnostic record.

| ODBC | Trino |
|---|---|
| `ASCII(s)` | `codepoint(CAST(substr(s,1,1) AS varchar(1)))` |
| `BIT_LENGTH(s)` | `(length(to_utf8(s))*8)` |
| `CHAR(n)` | `chr(n)` |
| `CHAR_LENGTH`, `CHARACTER_LENGTH`, `LENGTH` | `length` |
| `CONCAT`, `LCASE`->`lower`, `UCASE`->`upper`, `LTRIM`, `RTRIM`, `REPLACE`, `SUBSTRING`->`substr`, `SOUNDEX` | same-named Trino functions |
| `INSERT(s,st,len,s2)` | `concat(substr(s,1,st-1),s2,substr(s,st+len))` |
| `LEFT(s,n)` | `substr(s,1,n)` |
| `RIGHT(s,n)` | `substr(s, greatest(length(s)-(n)+1, 1))` |
| `LOCATE(a,b)` | `strpos(b,a)` |
| `LOCATE(a,b,s)` | `(CASE WHEN strpos(substr(b,s),a)=0 THEN 0 ELSE strpos(substr(b,s),a)+s-1 END)` |
| `OCTET_LENGTH(s)` | `length(to_utf8(s))` |
| `POSITION(a IN b)` | `position(a IN b)` |
| `REPEAT(s,n)` | `array_join(repeat(s,n),'')` |
| `SPACE(n)` | `array_join(repeat(' ',n),'')` |
| `ABS ACOS ASIN ATAN ATAN2 CEILING COS DEGREES EXP FLOOR MOD PI POWER RADIANS ROUND SIGN SIN SQRT TAN` | same-named Trino functions |
| `COT(x)` | `(1/tan(x))` |
| `LOG(x)` | `ln(x)` |
| `LOG10(x)` | `log10(x)` |
| `RAND([seed])` | `random()` (a seed is accepted but cannot be honoured) |
| `TRUNCATE(x[,n])` | `truncate(x[,n])` |
| `CURDATE()`, `CURRENT_DATE()` | `current_date` |
| `CURTIME([p])`, `CURRENT_TIME([p])` | `localtime` / `localtime(p)` |
| `NOW([p])`, `CURRENT_TIMESTAMP([p])` | `localtimestamp` / `localtimestamp(p)` |
| `DAYNAME(x)` | `date_format(x,'%W')` |
| `MONTHNAME(x)` | `date_format(x,'%M')` |
| `DAYOFMONTH(x)` | `day(x)` |
| `DAYOFWEEK(x)` | `((day_of_week(x) % 7) + 1)` (1 = Sunday, as ODBC requires) |
| `DAYOFYEAR(x)` | `day_of_year(x)` |
| `EXTRACT`, `HOUR`, `MINUTE`, `MONTH`, `QUARTER`, `SECOND`, `WEEK`, `YEAR` | same-named Trino functions |
| `TIMESTAMPADD(SQL_TSI_unit, n, t)` | `date_add('unit', n, t)` |
| `TIMESTAMPDIFF(SQL_TSI_unit, a, b)` | `date_diff('unit', a, b)` |
| `DATABASE()` | `current_catalog` |
| `USER()` | `current_user` |
| `IFNULL(a,b[,...])` | `coalesce(a,b[,...])` |
| `CONVERT(x, type)` | `CAST(x AS <trino type>)` (see above) |

`CURRENT_DATE`, `CURDATE`, `CURRENT_TIME`, `CURTIME`, `CURRENT_TIMESTAMP`, `NOW`, `PI`, `RAND`,
`DATABASE` and `USER` may also be written without parentheses (`{fn CURRENT_DATE}`).

`SQL_TIMEDATE_ADD_INTERVALS` / `SQL_TIMEDATE_DIFF_INTERVALS`: `SQL_TSI_FRAC_SECOND` (as
`millisecond`), `SECOND`, `MINUTE`, `HOUR`, `DAY`, `WEEK`, `MONTH`, `QUARTER`, `YEAR`.

### Conversion matrix (`SQL_CONVERT_<type>`)

Every type converts to the character types (`CHAR`, `VARCHAR`, `LONGVARCHAR`, `WCHAR`,
`WVARCHAR`, `WLONGVARCHAR`). Additionally: numerics and `BIT` to every numeric type and `BIT`;
character types to everything (numerics, date, time, timestamp, binary, GUID, intervals); `DATE`
and `TIMESTAMP` to `DATE`/`TIMESTAMP`; `TIME` to `TIME`; binary types to binary types; `GUID` to
`GUID`; each interval class to itself.

## Exported functions (`SQLGetFunctions`)

All ODBC 3.8 core functions plus the level-1/level-2 catalog and parameter functions:
`SQLAllocHandle`, `SQLFreeHandle`, `SQLAllocEnv/Connect/Stmt`, `SQLFreeEnv/Connect/Stmt`,
`SQLConnect`, `SQLDriverConnect`, `SQLBrowseConnect`, `SQLDisconnect`, `SQLEndTran`, `SQLTransact`,
`SQLGetInfo`, `SQLGetFunctions`, `SQLGetTypeInfo`, `SQLSet/GetEnvAttr`, `SQLSet/GetConnectAttr`,
`SQLSet/GetConnectOption`, `SQLSet/GetStmtAttr`, `SQLSet/GetStmtOption`, `SQLPrepare`, `SQLExecute`,
`SQLExecDirect`, `SQLNativeSql`, `SQLNumParams`, `SQLDescribeParam`, `SQLBindParameter`,
`SQLSetParam`, `SQLParamOptions`, `SQLParamData`, `SQLPutData`, `SQLCancel`, `SQLCancelHandle`,
`SQLRowCount`, `SQLMoreResults`, `SQLCloseCursor`, `SQLGet/SetCursorName`, `SQLSetPos`,
`SQLBulkOperations` (returns `HYC00`), `SQLSetScrollOptions` (returns `HYC00`), `SQLBindCol`,
`SQLFetch`, `SQLFetchScroll`, `SQLExtendedFetch`, `SQLGetData`, `SQLNumResultCols`, `SQLDescribeCol`,
`SQLColAttribute`, `SQLColAttributes`, `SQLTables`, `SQLColumns`, `SQLPrimaryKeys`, `SQLForeignKeys`,
`SQLStatistics`, `SQLSpecialColumns`, `SQLProcedures`, `SQLProcedureColumns`, `SQLTablePrivileges`,
`SQLColumnPrivileges`, `SQLGetDiagRec`, `SQLGetDiagField`, `SQLError`, `SQLCopyDesc`,
`SQLGet/SetDescField`, `SQLGet/SetDescRec`, and the `W` (Unicode) variant of every function that
takes strings. `SQLDataSources`, `SQLDrivers` and `SQLCompleteAsync` belong to the driver manager.

## Connection and statement attributes

Accepted: `SQL_ATTR_ODBC_VERSION` (2, 3, 3.80), `SQL_ATTR_CONNECTION_POOLING`, `SQL_ATTR_CP_MATCH`,
`SQL_ATTR_ACCESS_MODE`, `SQL_ATTR_AUTOCOMMIT`, `SQL_ATTR_CONNECTION_TIMEOUT`,
`SQL_ATTR_LOGIN_TIMEOUT` (a non-zero value set before connecting overrides `ConnectTimeout`; it
may also be set on an open connection, where it is stored for `SQLGetConnectAttr` and otherwise
ignored), `SQL_ATTR_CURRENT_CATALOG` (runs `USE "catalog"` when connected), `SQL_ATTR_METADATA_ID`, `SQL_ATTR_QUIET_MODE`, `SQL_ATTR_TRACE`,
`SQL_ATTR_TRACEFILE`, `SQL_ATTR_TXN_ISOLATION`, `SQL_ATTR_ANSI_APP`, `SQL_ATTR_RESET_CONNECTION`,
`SQL_ATTR_AUTO_IPD` (read), `SQL_ATTR_CONNECTION_DEAD` (read), `SQL_ATTR_QUERY_TIMEOUT`,
`SQL_ATTR_MAX_ROWS`, `SQL_ATTR_MAX_LENGTH`, `SQL_ATTR_NOSCAN`, `SQL_ATTR_RETRIEVE_DATA`,
`SQL_ATTR_ENABLE_AUTO_IPD`, `SQL_ATTR_CURSOR_SENSITIVITY` (unspecified/insensitive), the row and
parameter binding attributes, `SQL_ATTR_ROW_NUMBER` (read), `SQL_ATTR_IMP_ROW_DESC` /
`SQL_ATTR_IMP_PARAM_DESC` (read).

Downgraded with `01S02`: `SQL_ATTR_PACKET_SIZE`, `SQL_ATTR_ASYNC_ENABLE`,
`SQL_ATTR_ASYNC_DBC_FUNCTIONS`, non-forward-only cursor types, `SQL_ATTR_CURSOR_SCROLLABLE`,
sensitive cursors, concurrency other than read-only, `SQL_ATTR_SIMULATE_CURSOR`,
`SQL_ATTR_USE_BOOKMARKS`, a non-zero `SQL_ATTR_KEYSET_SIZE`.

Refused with `HYC00`: `SQL_ATTR_ENLIST_IN_DTC`, `SQL_ATTR_TRANSLATE_LIB` / `OPTION`,
`SQL_ATTR_DBC_INFO_TOKEN`, `SQL_ATTR_ASYNC_DBC_EVENT`, `SQL_ATTR_ASYNC_STMT_EVENT`,
`SQL_ATTR_FETCH_BOOKMARK_PTR`. Invalid values of an accepted attribute: `HY024`. Unknown or
read-only attributes: `HY092`.
