# ODBC API layer design notes (src/odbc/*.cpp)

All entry points: `TRINO_ODBC_API SQLRETURN SQL_API SQLxxx(...)` declared `extern "C"`. Body = guard*(handle, "SQLxxx",
[&](T& h) -> SQLRETURN { ... }). Text arguments are converted with textIn()/TextOut (api_common.h) and forwarded to the
`impl::` functions so ANSI and Unicode share one implementation. ANSI entry points live next to their impl; every *W entry
point lives in api_unicode.cpp.

## api_common.cpp
guard*: validate handle (magic + HandleRegistry) -> SQL_INVALID_HANDLE; lock; `if(!keepDiag) diag.clear()`; run body;
catch DriverError/TrinoQueryError/HttpError/CancelledError/std::bad_alloc(HY001)/std::exception(HY000) -> diag; set
diag.returnCode; TRINO_LOG_TRACE("odbc", "SQLxxx -> rc"). For statements also lock nothing on the connection (statement
mutex only) — but connect/disconnect lock the connection and every statement. textInBytes: wide length in bytes -> units.
TextOut::write: narrow -> utf8ToNarrow + copyNarrowOut; wide -> copyWideOutChars / copyWideOutBytes.

## api_handles.cpp
SQLAllocHandle(ENV/DBC/STMT/DESC), SQLFreeHandle, SQLAllocEnv/Connect/Stmt, SQLFreeEnv/Connect. ENV: SQL_ATTR_ODBC_VERSION
must be set before DBC alloc (HY010). DBC alloc registers with env; STMT alloc requires connected (08003); DESC alloc creates
explicit ARD-kind descriptor owned by the connection. Free STMT closes cursor, cancels; free DBC when connected -> HY010;
free ENV with connections -> HY010; free explicit DESC reverts statements using it to implicit ones. TrinoClient::globalInit()
on first env alloc; Logger::configureFromEnvironment().

## api_connect.cpp
SQLConnect(A)+impl::connect: builds config from DSN (loadDsn) + uid/pwd override; SQLDriverConnect(A)+impl::driverConnect:
parse string, `DSN=` -> loadDsn(onlyMissing), then finalize/validate; completion SQL_DRIVER_PROMPT / COMPLETE /
COMPLETE_REQUIRED: on Windows call the setup dialog (declared in src/platform/win/setup.h as
`bool showConnectDialog(HWND, ConnectionConfig&)`; guard with #ifdef TRINO_ODBC_WINDOWS) when required values are missing,
elsewhere behave like SQL_DRIVER_NOPROMPT; output string = config.toConnectionString(false) — the full string including
password must be returned when the app gave it (ODBC requires the out string usable to reconnect: include PWD as given).
Already connected -> 08002. Connect flow: config.finalize(); validate(); Logger.configure(logLevel, logPath); session init
from config (catalog/schema/props/roles/tz/etc.); client = TrinoClient; client->connect(); connected=true; serverVersion.
Map HttpError 28000 -> 28000, timeouts -> HYT01 during connect, other -> 08001.
SQLBrowseConnect(A)+impl::browseConnect: first call with missing Host/UID -> SQL_NEED_DATA and an out string listing
`Host:Host=?;Port:Port=?;UID:User ID=?;PWD:Password=?;Catalog:Catalog=?;Schema:Schema=?;SSL:SSL=?`; when complete connect.
SQLDisconnect: open statements with cursors -> close them (do not fail), cancelAll, client.reset(), connected=false; when
a driver transaction is open -> 25000. SQLEndTran(ENV: for every connection; DBC): autocommit on -> success no-op; with
EnableTransactions: run `COMMIT`/`ROLLBACK` via executeToMemory with ExecOptions.transactionId, clear session tx, inTransaction=false;
without -> HYC00 when a transaction would be needed. SQLTransact -> SQLEndTran.

## api_attrs.cpp (env/dbc/stmt attributes, ODBC2 option wrappers)
Env: SQL_ATTR_ODBC_VERSION (2/3/3.80), SQL_ATTR_CONNECTION_POOLING, SQL_ATTR_CP_MATCH, SQL_ATTR_OUTPUT_NTS (only SQL_TRUE).
Dbc set: SQL_ATTR_ACCESS_MODE, SQL_ATTR_AUTOCOMMIT (OFF -> HYC00 unless enableTransactions), SQL_ATTR_CONNECTION_TIMEOUT,
SQL_ATTR_LOGIN_TIMEOUT (before connect; stored, feeds connectTimeout), SQL_ATTR_CURRENT_CATALOG (string; connected -> execute
`USE "<catalog>"` (quote), else store pendingCatalog applied at connect), SQL_ATTR_METADATA_ID, SQL_ATTR_PACKET_SIZE (01S02),
SQL_ATTR_QUIET_MODE, SQL_ATTR_TRACE/TRACEFILE (accept), SQL_ATTR_TXN_ISOLATION (accept all four; store), SQL_ATTR_ASYNC_ENABLE
(ON -> 01S02 stays off), SQL_ATTR_ODBC_CURSORS (DM), SQL_ATTR_ENLIST_IN_DTC (HYC00), SQL_ATTR_ANSI_APP (accept),
SQL_ATTR_RESET_CONNECTION (accept), SQL_ATTR_TRANSLATE_LIB/OPTION (HYC00), unknown -> HY092.
Dbc get: same + SQL_ATTR_AUTO_IPD (SQL_TRUE), SQL_ATTR_CONNECTION_DEAD (SQL_CD_FALSE when connected else TRUE),
SQL_ATTR_CURRENT_CATALOG (session catalog), SQL_ATTR_DBC_INFO_TOKEN (HYC00). Strings honour `wide`.
Stmt set/get: SQL_ATTR_APP_ROW_DESC/APP_PARAM_DESC (SQL_NULL_HDESC -> implicit; explicit desc must belong to same connection
else HY024; IRD/IPD set -> HY017), SQL_ATTR_IMP_ROW_DESC/IMP_PARAM_DESC (get only), SQL_ATTR_ASYNC_ENABLE (01S02),
SQL_ATTR_CONCURRENCY (READ_ONLY only; others 01S02), SQL_ATTR_CURSOR_SCROLLABLE (SCROLLABLE -> 01S02), SQL_ATTR_CURSOR_SENSITIVITY
(accept UNSPECIFIED/INSENSITIVE, SENSITIVE -> 01S02), SQL_ATTR_CURSOR_TYPE (FORWARD_ONLY; others 01S02 -> forward only),
SQL_ATTR_ENABLE_AUTO_IPD, SQL_ATTR_FETCH_BOOKMARK_PTR (HYC00), SQL_ATTR_KEYSET_SIZE (0 only), SQL_ATTR_MAX_LENGTH (store;
apply to char/binary GetData/Fetch truncation without warning per spec), SQL_ATTR_MAX_ROWS, SQL_ATTR_METADATA_ID, SQL_ATTR_NOSCAN,
SQL_ATTR_PARAM_BIND_OFFSET_PTR/PARAM_BIND_TYPE/PARAM_OPERATION_PTR/PARAM_STATUS_PTR/PARAMS_PROCESSED_PTR/PARAMSET_SIZE (-> apd/ipd
headers), SQL_ATTR_QUERY_TIMEOUT, SQL_ATTR_RETRIEVE_DATA, SQL_ATTR_ROW_ARRAY_SIZE/ROWSET_SIZE (ODBC2, -> ard.arraySize),
SQL_ATTR_ROW_BIND_OFFSET_PTR/ROW_BIND_TYPE/ROW_OPERATION_PTR/ROW_STATUS_PTR/ROWS_FETCHED_PTR (-> ard/ird headers),
SQL_ATTR_ROW_NUMBER (get only), SQL_ATTR_SIMULATE_CURSOR (01S02), SQL_ATTR_USE_BOOKMARKS (OFF only, else 01S02).
Setting cursor attributes while a cursor is open -> 24000. SQLSet/GetConnectOption & SQLSet/GetStmtOption forward to the
attr functions (with the ODBC 2 pointer conventions; SQL_ROWSET_SIZE -> SQL_ATTR_ROW_ARRAY_SIZE).

## api_diag.cpp
SQLGetDiagRec(A)+impl::getDiagRec (rec<1 -> SQL_ERROR; beyond count -> SQL_NO_DATA; sqlstate buffer 6 chars),
SQLGetDiagField(A)+impl::getDiagField: header fields SQL_DIAG_CURSOR_ROW_COUNT, SQL_DIAG_DYNAMIC_FUNCTION(_CODE),
SQL_DIAG_NUMBER, SQL_DIAG_RETURNCODE, SQL_DIAG_ROW_COUNT (statement only; else HY092/SQL_ERROR); record fields SQL_DIAG_CLASS_ORIGIN
("ISO 9075" or "ODBC 3.0" for IM/HY), SQL_DIAG_COLUMN_NUMBER, SQL_DIAG_CONNECTION_NAME, SQL_DIAG_MESSAGE_TEXT, SQL_DIAG_NATIVE,
SQL_DIAG_ROW_NUMBER, SQL_DIAG_SERVER_NAME, SQL_DIAG_SQLSTATE, SQL_DIAG_SUBCLASS_ORIGIN. Uses diagFor()/mutexFor(); never clears
the diag it reads. SQLError (ODBC2): walks the records of the "most specific" handle given, one per call. The position is a
private index kept outside `Diag` in a mutex-guarded `std::map<const Diag*, size_t>` (api_common.cpp); the guards reset it
whenever they clear a diagnostic area.

## api_desc.cpp
SQLCopyDesc (IRD target -> HY016), SQLGetDescField(A)/SQLSetDescField(A)/SQLGetDescRec(A)/SQLSetDescRec + impls with the
full field list from ARCHITECTURE.md §8 (header + record), consistency checks (SQL_DESC_TYPE/CONCISE_TYPE/DATETIME_INTERVAL_CODE
kept consistent via conciseTypeOf/verboseTypeOf; setting SQL_DESC_DATA_PTR marks bound; IRD read-only except through
SQLDescribeCol path -> HY016; record index 0 (bookmark) -> 07009 for ARD data, header fields need rec 0).

## api_info.cpp — SQLGetInfo & SQLGetFunctions
SQLGetInfo answers every info type. Character values via TextOut (bytes convention); SQLUSMALLINT/SQLUINTEGER/SQLULEN written with the
correct width; when `value` is null still set *strLen. Key values (others per spec, chosen to describe Trino truthfully):
SQL_DRIVER_NAME kDriverFileName; SQL_DRIVER_VER kDriverVersionOdbc; SQL_DRIVER_ODBC_VER "03.80"; SQL_DBMS_NAME "Trino";
SQL_DBMS_VER "<major>.<minor>.<patch> Trino <version>" formatted "%02d.%02d.%04d" from the numeric Trino version
(e.g. 483 -> "483.00.0000") or "00.00.0000" when unknown; SQL_SERVER_NAME host; SQL_DATA_SOURCE_NAME dsn; SQL_USER_NAME user;
SQL_DATABASE_NAME current catalog; SQL_CATALOG_NAME "Y"; SQL_CATALOG_TERM "catalog"; SQL_SCHEMA_TERM "schema"; SQL_TABLE_TERM
"table"; SQL_PROCEDURE_TERM "procedure"; SQL_CATALOG_NAME_SEPARATOR "."; SQL_CATALOG_LOCATION SQL_CL_START; SQL_CATALOG_USAGE
SQL_CU_DML_STATEMENTS|SQL_CU_TABLE_DEFINITION|SQL_CU_INDEX_DEFINITION|SQL_CU_PRIVILEGE_DEFINITION; SQL_SCHEMA_USAGE same set;
SQL_IDENTIFIER_QUOTE_CHAR "\""; SQL_IDENTIFIER_CASE SQL_IC_LOWER; SQL_QUOTED_IDENTIFIER_CASE SQL_IC_SENSITIVE;
SQL_SPECIAL_CHARACTERS ""; SQL_MAX_IDENTIFIER_LEN 128 (and every other SQL_MAX_* 0 = unlimited, SQL_MAX_COLUMNS_IN_* 0);
SQL_SEARCH_PATTERN_ESCAPE "\\"; SQL_LIKE_ESCAPE_CLAUSE "Y"; SQL_SQL_CONFORMANCE SQL_SC_SQL92_ENTRY; SQL_ODBC_INTERFACE_CONFORMANCE
SQL_OIC_CORE; SQL_ODBC_API_CONFORMANCE SQL_OAC_LEVEL1; SQL_ODBC_SQL_CONFORMANCE SQL_OSC_CORE; SQL_STANDARD_CLI_CONFORMANCE
SQL_SCC_ISO92_CLI; SQL_ACCESSIBLE_TABLES "Y"; SQL_ACCESSIBLE_PROCEDURES "N"; SQL_PROCEDURES "N"; SQL_MULT_RESULT_SETS "N";
SQL_MULTIPLE_ACTIVE_TXN "N"; SQL_NEED_LONG_DATA_LEN "N"; SQL_ROW_UPDATES "N"; SQL_DATA_SOURCE_READ_ONLY per config.readOnly;
SQL_TXN_CAPABLE enableTransactions?SQL_TC_DML:SQL_TC_NONE; SQL_DEFAULT_TXN_ISOLATION SQL_TXN_READ_UNCOMMITTED;
SQL_TXN_ISOLATION_OPTION all four; SQL_CURSOR_COMMIT_BEHAVIOR/ROLLBACK SQL_CB_PRESERVE; SQL_CURSOR_SENSITIVITY SQL_UNSPECIFIED;
SQL_SCROLL_OPTIONS SQL_SO_FORWARD_ONLY; SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1 SQL_CA1_NEXT|SQL_CA1_LOCK_NO_CHANGE|SQL_CA1_POS_POSITION;
SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2 SQL_CA2_READ_ONLY_CONCURRENCY|SQL_CA2_MAX_ROWS_SELECT;
STATIC/KEYSET/DYNAMIC attributes 0; SQL_BOOKMARK_PERSISTENCE 0; SQL_GETDATA_EXTENSIONS SQL_GD_ANY_COLUMN|SQL_GD_ANY_ORDER|SQL_GD_BLOCK|SQL_GD_BOUND;
SQL_ASYNC_MODE SQL_AM_NONE; SQL_ASYNC_DBC_FUNCTIONS SQL_ASYNC_DBC_NOT_CAPABLE; SQL_ASYNC_NOTIFICATION not capable;
SQL_DRIVER_AWARE_POOLING_SUPPORTED not capable; SQL_BATCH_SUPPORT 0; SQL_BATCH_ROW_COUNT 0; SQL_PARAM_ARRAY_ROW_COUNTS SQL_PARC_NO_BATCH;
SQL_PARAM_ARRAY_SELECTS SQL_PAS_NO_SELECT; SQL_DESCRIBE_PARAMETER "Y"; SQL_CONCAT_NULL_BEHAVIOR SQL_CB_NULL; SQL_NULL_COLLATION SQL_NC_END;
SQL_COLUMN_ALIAS "Y"; SQL_CORRELATION_NAME SQL_CN_ANY; SQL_EXPRESSIONS_IN_ORDERBY "Y"; SQL_ORDER_BY_COLUMNS_IN_SELECT "N";
SQL_GROUP_BY SQL_GB_GROUP_BY_CONTAINS_SELECT; SQL_OUTER_JOINS "Y"; SQL_OJ_CAPABILITIES SQL_OJ_LEFT|RIGHT|FULL|NESTED|NOT_ORDERED|INNER|ALL_COMPARISON_OPS;
SQL_SQL92_RELATIONAL_JOIN_OPERATORS SQL_SRJO_CROSS_JOIN|EXCEPT_JOIN|FULL_OUTER_JOIN|INNER_JOIN|INTERSECT_JOIN|LEFT_OUTER_JOIN|RIGHT_OUTER_JOIN (no NATURAL/UNION join);
SQL_SQL92_PREDICATES BETWEEN|COMPARISON|EXISTS|IN|ISNOTNULL|ISNULL|LIKE|QUANTIFIED_COMPARISON (no MATCH/UNIQUE/OVERLAPS);
SQL_SQL92_VALUE_EXPRESSIONS SQL_SVE_CASE|CAST|COALESCE|NULLIF; SQL_SQL92_NUMERIC_VALUE_FUNCTIONS EXTRACT|POSITION;
SQL_SQL92_STRING_FUNCTIONS LOWER|UPPER|SUBSTRING|TRIM_BOTH|TRIM_LEADING|TRIM_TRAILING (no CONVERT); SQL_SQL92_DATETIME_FUNCTIONS CURRENT_DATE|CURRENT_TIME|CURRENT_TIMESTAMP;
SQL_SQL92_ROW_VALUE_CONSTRUCTOR SQL_SRVC_VALUE_EXPRESSION|NULL|ROW_SUBQUERY; SQL_SQL92_GRANT/REVOKE 0; SQL_SQL92_FOREIGN_KEY_* 0;
SQL_AGGREGATE_FUNCTIONS SQL_AF_ALL|AVG|COUNT|DISTINCT|MAX|MIN|SUM; SQL_STRING_FUNCTIONS/NUMERIC/TIMEDATE/SYSTEM/CONVERT bitmasks exactly the sets in IMPLEMENTATION-NOTES.md;
SQL_TIMEDATE_ADD_INTERVALS & SQL_TIMEDATE_DIFF_INTERVALS FRAC_SECOND|SECOND|MINUTE|HOUR|DAY|WEEK|MONTH|QUARTER|YEAR;
SQL_CONVERT_<type> bitmasks: every type converts to VARCHAR/WVARCHAR/CHAR/WCHAR/LONGVARCHAR/WLONGVARCHAR; numerics to all numerics + BIT;
char to everything; date/timestamp to DATE/TIMESTAMP/char; time to TIME/char; binary to binary/char; GUID to char/GUID;
SQL_SUBQUERIES SQL_SQ_CORRELATED_SUBQUERIES|COMPARISON|EXISTS|IN|QUANTIFIED; SQL_UNION SQL_U_UNION|SQL_U_UNION_ALL;
SQL_DATETIME_LITERALS DATE|TIME|TIMESTAMP|INTERVAL_YEAR|MONTH|DAY|HOUR|MINUTE|SECOND|YEAR_TO_MONTH|DAY_TO_HOUR|DAY_TO_MINUTE|DAY_TO_SECOND|HOUR_TO_MINUTE|HOUR_TO_SECOND|MINUTE_TO_SECOND;
SQL_KEYWORDS: Trino reserved words comma-separated (ALTER,AND,AS,BETWEEN,BY,CASE,CAST,CONSTRAINT,CREATE,CROSS,CUBE,CURRENT_CATALOG,CURRENT_DATE,CURRENT_PATH,CURRENT_ROLE,CURRENT_SCHEMA,CURRENT_TIME,CURRENT_TIMESTAMP,CURRENT_USER,DEALLOCATE,DELETE,DESCRIBE,DISTINCT,DROP,ELSE,END,ESCAPE,EXCEPT,EXECUTE,EXISTS,EXTRACT,FALSE,FOR,FROM,FULL,GROUP,GROUPING,HAVING,IN,INNER,INSERT,INTERSECT,INTO,IS,JOIN,JSON_ARRAY,JSON_EXISTS,JSON_OBJECT,JSON_QUERY,JSON_TABLE,JSON_VALUE,LEFT,LIKE,LISTAGG,LOCALTIME,LOCALTIMESTAMP,NATURAL,NORMALIZE,NOT,NULL,ON,OR,ORDER,OUTER,PREPARE,RECURSIVE,RIGHT,ROLLUP,SELECT,SKIP,TABLE,THEN,TRIM,TRUE,UESCAPE,UNION,UNNEST,USING,VALUES,WHEN,WHERE,WITH);
SQL_INFO_SCHEMA_VIEWS SQL_ISV_COLUMNS|SQL_ISV_SCHEMATA|SQL_ISV_TABLES|SQL_ISV_VIEWS; SQL_DDL_INDEX 0; SQL_CREATE_TABLE SQL_CT_CREATE_TABLE;
SQL_CREATE_SCHEMA SQL_CS_CREATE_SCHEMA; SQL_CREATE_VIEW SQL_CV_CREATE_VIEW; SQL_DROP_TABLE SQL_DT_DROP_TABLE; SQL_DROP_VIEW SQL_DV_DROP_VIEW; SQL_DROP_SCHEMA SQL_DS_DROP_SCHEMA;
SQL_ALTER_TABLE SQL_AT_ADD_COLUMN|SQL_AT_DROP_COLUMN; SQL_INSERT_STATEMENT SQL_IS_INSERT_LITERALS|SQL_IS_INSERT_SEARCHED;
SQL_INTEGRITY "N"; SQL_FILE_USAGE SQL_FILE_NOT_SUPPORTED; SQL_DTC_TRANSITION_COST 0; SQL_ACTIVE_ENVIRONMENTS 0; SQL_MAX_CONCURRENT_ACTIVITIES 0; SQL_MAX_DRIVER_CONNECTIONS 0;
SQL_COLLATION_SEQ ""; SQL_LOCK_TYPES SQL_LCK_NO_CHANGE; SQL_POS_OPERATIONS SQL_POS_POSITION; SQL_POSITIONED_STATEMENTS 0; SQL_STATIC_SENSITIVITY 0;
SQL_ODBC_SAG_CLI_CONFORMANCE SQL_OSCC_COMPLIANT; SQL_FETCH_DIRECTION SQL_FD_FETCH_NEXT; SQL_SCROLL_CONCURRENCY SQL_SCCO_READ_ONLY;
SQL_NON_NULLABLE_COLUMNS SQL_NNC_NON_NULL; SQL_DRIVER_HDBC/HENV/HSTMT/HDESC handled by the DM (never asked; return HY096 if seen);
SQL_XOPEN_CLI_YEAR "1995"; SQL_OUTER_JOINS "Y"; SQL_CONVERT_FUNCTIONS SQL_FN_CVT_CAST|SQL_FN_CVT_CONVERT; SQL_CREATE_TRANSLATION 0 etc.
Unknown info type -> HY096. Helper: `std::string dbmsVersionString(const Connection&)`.
SQLGetFunctions: SQL_API_ODBC3_ALL_FUNCTIONS bitmap (SQL_FUNC_EXISTS) listing exactly the exported functions;
SQL_API_ALL_FUNCTIONS (100-entry ODBC2 array); single function ids.

## api_statement.cpp
SQLPrepare(A)+impl::prepare: requireNoCursor; s.originalSql; translate escapes unless noscan; paramCount; prepName =
"odbc_stmt_" + counter; invalidate described caches; state Prepared. SQLExecDirect(A)+impl::execDirect = prepare + execute
(state Allocated on failure). SQLExecute: state must be Prepared/ExecutedNoCursor/CursorOpen-after-close..., cursor open ->
24000; impl::executeStatement:
 1. paramset size N = apd->header.arraySize; params processed ptr / status ptr from ipd header. For set k: build ParamValue
    for each param from APD/IPD records (bind offsets, bind type row-wise: address = dataPtr + k*bindType or element size,
    indicator/octet length pointers likewise); SQL_NULL_DATA -> null; SQL_DATA_AT_EXEC / SQL_LEN_DATA_AT_EXEC -> enter
    NeedData state: return SQL_NEED_DATA; SQLParamData returns the dataPtr token of the next DAE param (ValuePtrPtr) and,
    once all provided, continues execution and returns its result; SQLPutData appends bytes (SQL_NTS handling for char).
    Wrong param count vs bound (fewer bound than markers) -> 07002.
 2. SQL to send: if paramCount == 0 -> s.sql; else literals via renderParameterLiteral(value, opts, describedInput type when
    valid) and either `EXECUTE IMMEDIATE '<escaped sql>' USING a, b` or (explicitPrepare) ExecOptions.preparedStatements
    {prepName: s.sql} + `EXECUTE <prepName> USING a, b`.
 3. readOnly config and DML/DDL -> DriverError 42000 "connection is read-only". Transactions: enableTransactions && autocommit
    off && session.transactionId empty && !classify.isTransaction -> opts.startTransaction=true and conn.inTransaction=true after.
 4. opts.queryTimeoutSec = s.queryTimeout; opts.maxRows = s.maxRows. Run `conn.client->execute(sql, opts)` with
    s.setActiveQuery(shared) so SQLCancel can reach it; on CancelledError -> HY008.
 5. hasResultSet -> result = ResultSet(move(query)); result->setMaxRows; rebuildIrd; rowCount=-1; state CursorOpen; getData reset;
    else rowCount = updateCount.value_or(classify.isDml ? 0 : -1); state ExecutedNoCursor; diag.rowCount.
    Paramset > 1 with result set -> HYC00 (after first set). Param status array entries SQL_PARAM_SUCCESS/ERROR.
 6. Warnings from the query -> diag 01000 records ([Trino] warning text) -> SQL_SUCCESS_WITH_INFO.
ensureDescribedOutput: internal executeToMemory("DESCRIBE OUTPUT <prepName>") with opts.preparedStatements {prepName: sql};
rows: Column Name, Catalog, Schema, Table, Type, Type Size, Aliased -> describedColumns (ResultColumn::make with catalog/schema/
table/aliased). Trino describes DML/DDL either with zero rows or with a nameless "unknown" placeholder, so
describedOutputIsResultSet=false when there are no rows, no named/typed column, or the statement kind is DML/DDL/transaction/
USE/SET. ensureDescribedInput: "DESCRIBE INPUT <prepName>" rows: Position, Type ->
describedParams (Type "unknown" -> TypeCategory::Unknown). Both cached until re-prepare; failures propagate as diag.
SQLNumParams (needs Prepared+): paramCount. SQLDescribeParam: ensureDescribedInput; map type via mapTrinoType -> DataType,
ParameterSize, DecimalDigits, Nullable SQL_NULLABLE_UNKNOWN; unknown -> SQL_WVARCHAR/L. SQLBindParameter: validate
InputOutputType (SQL_PARAM_INPUT only; others HYC00), C type (HY003), SQL type (HY004), param number (HY093 when 0), writes
APD record (conciseType, type, octetLength=BufferLength, dataPtr, octetLengthPtr=indicatorPtr=StrLen_or_IndPtr) and IPD
record (sqlType, columnSize->length/precision, decimalDigits->scale, parameterType). SQLSetParam -> SQLBindParameter with
SQL_PARAM_INPUT_OUTPUT semantics per ODBC2 (treat as input). SQLParamOptions -> apd arraySize + rowsProcessedPtr (ipd).
SQLCancel: does NOT take the statement mutex. With a function in progress on another thread it sets s.cancelRequested and
calls activeQuery->cancel(); otherwise it only try-locks the statement and, in NeedData state, abandons the data-at-execution
sequence; it returns SQL_SUCCESS. SQLCancelHandle: STMT -> SQLCancel;
DBC -> cancelAll. SQLNativeSql(A)+impl. SQLRowCount. SQLMoreResults: cursor/DML present -> close & SQL_NO_DATA; else HY010.
SQLCloseCursor: no cursor -> 24000; SQLFreeStmt: SQL_CLOSE (no error when no cursor), SQL_DROP (-> free), SQL_UNBIND,
SQL_RESET_PARAMS. SQLGet/SetCursorName(A)+impl: default name "SQL_CUR<n>" generated at alloc (cursorNameExplicit=false);
set while cursor open -> 24000; empty, longer than 128 characters, or starting with the reserved prefixes "SQLCUR"/"SQL_CUR"
-> 34000; a name already used by another statement of the connection -> 3C000.
SQLSetPos(SQL_POSITION, lock SQL_LOCK_NO_CHANGE): rowsetPos = RowNumber (1..rowsetSize; 0, the whole rowset, is rejected with
HY109 for SQL_POSITION); other operations -> HYC00. SQLBulkOperations -> HYC00. SQLSetScrollOptions -> HYC00.

## api_results.cpp
SQLBindCol: col 0 -> 07009 (bookmarks off); C type validation HY003; BufferLength < 0 -> HY090; TargetValue null and
indicator null -> unbind record; writes ARD record (conciseType/type/subcode via normalizeCType, octetLength=BufferLength,
dataPtr, octetLengthPtr=indicatorPtr=StrLen_or_Ind); ard count grows. SQLNumResultCols: after Prepared -> ensureDescribedOutput
(0 columns when not a result set); after execute -> result columns. SQLDescribeCol(A)+impl: col range 07009, name via TextOut
(01004 on truncation), DataType = concise, ColumnSize, DecimalDigits, Nullable. SQLColAttribute(A)+impl: every SQL_DESC_*
and SQL_COLUMN_* (ODBC2 ids: SQL_COLUMN_COUNT, SQL_COLUMN_NAME, SQL_COLUMN_TYPE, SQL_COLUMN_LENGTH, SQL_COLUMN_PRECISION,
SQL_COLUMN_SCALE, SQL_COLUMN_DISPLAY_SIZE, SQL_COLUMN_NULLABLE, SQL_COLUMN_UNSIGNED, SQL_COLUMN_MONEY, SQL_COLUMN_UPDATABLE,
SQL_COLUMN_AUTO_INCREMENT, SQL_COLUMN_CASE_SENSITIVE, SQL_COLUMN_SEARCHABLE, SQL_COLUMN_TYPE_NAME, SQL_COLUMN_TABLE_NAME,
SQL_COLUMN_OWNER_NAME, SQL_COLUMN_QUALIFIER_NAME, SQL_COLUMN_LABEL) from the IRD record; character attrs via TextOut with the
byte convention (bufLen in bytes), numeric into *numAttr (SQL_DESC_COUNT works with col 0). SQLColAttributes -> same.
impl::fetchRowset (SQLFetch, SQLFetchScroll SQL_FETCH_NEXT only else HY106, SQLExtendedFetch with its own status/rows args):
requireCursor; rowset size = ard->header.arraySize; rows = result->fetchRowset(n); if 0 -> state stays (cursor after last) return
SQL_NO_DATA and set rowsFetched=0, status array entries SQL_ROW_NOROW. Else for each row i: for each bound ARD record c
(dataPtr or indicatorPtr): address arithmetic per SQL_DESC_BIND_TYPE (column-wise: ptr + i*elementSize where elementSize =
octetLength for char/binary, sizeof(type) for fixed; row-wise: ptr + i*bindType) + *bindOffsetPtr; target.numericPrecision/scale
from ARD record when precisionSet/scaleSet; maxLength applies to char/binary targets by capping bufferLength (no diag);
convertToC(..., chunkOffset=nullptr, diag, rowNumber=i+1, columnNumber=c); status[i] = SUCCESS / SUCCESS_WITH_INFO / ERROR;
rowsFetchedPtr; s.rowNumber = rowsetStartRowNumber; rowsetPos=1; getData.reset(columnCount); state Fetched. Return SQL_ERROR
only when every row failed (else SUCCESS_WITH_INFO with 01S01 records already added per row). retrieveData==SQL_RD_OFF -> no
conversions. SQLGetData: state Fetched required (24000), col 0 -> 07009, col > count -> 07009, C type HY003, bufferLength<0 ->
HY090 (for char/binary), uses getData.offsets[col-1] as chunkOffset; row = result->rowsetRow(rowsetPos-1); maxLength cap;
returns per convertToC. SQLNumResultCols/SQLDescribeCol on Allocated -> HY010.

## api_catalog.cpp & api_unicode.cpp
Catalog ANSI entry points build CatalogArgs (null pointer -> nullopt; NameLength SQL_NTS or count; identifiersAreLiterals =
s.metadataId==SQL_TRUE || config.metadataCaseSensitive) and call impl::xxx which: requireConnected; requireNoCursor (24000);
closes previous result; calls catalog::catalogXxx(conn, args); installCatalogResult(s, rows, "SQLTables"): result = ResultSet(
move(rows)), rebuildIrd, rowCount=-1, state CursorOpen, diag.dynamicFunction. SQLGetTypeInfo(A): validate type (HY004) then
catalogTypeInfo. api_unicode.cpp: every *W function converting inputs with textIn(SQLWCHAR*) and outputs with TextOut(wide,
lengthInBytes as the spec says: SQLGetInfoW, SQLColAttributeW, SQLColAttributesW, SQLGetConnectAttrW/OptionW,
SQLGetStmtAttrW, SQLGetDescFieldW use bytes; SQLDriverConnectW, SQLBrowseConnectW, SQLGetDiagRecW, SQLDescribeColW,
SQLGetCursorNameW, SQLNativeSqlW, SQLGetDescRecW, SQLErrorW use characters; SQLGetDiagFieldW: bytes for strings).
SQLSetConnectAttrW/SQLSetStmtAttrW/SQLSetDescFieldW pass wide=true with the byte length.
