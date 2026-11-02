# Implementation notes: coding rules and shared interfaces

Contributor notes that complement [`ARCHITECTURE.md`](../../ARCHITECTURE.md). Read that document
first, then the headers a change touches. The headers under `src/` listed in ARCHITECTURE.md §3 are
stable interfaces shared by several modules: change them only when necessary, minimally, and keep
every other module compiling.

## Coding rules

* Production quality: every error path handled, no TODO stubs, no placeholder returns.
* Namespace `trino_odbc`. Never `printf`/`cout`; use `TRINO_LOG_*`. Comments say *why*.
* Portability: the code must compile with MSVC, GCC and Apple clang. No VLAs, no `__attribute__`
  without guards, POSIX calls only under `#ifdef TRINO_ODBC_UNIX`, Windows calls under
  `#ifdef TRINO_ODBC_WINDOWS`. `SQLWCHAR` may be 2 or 4 bytes — never assume.
* Keep the build warning-free: fix every warning in the files you change. A quick
  syntax-only check of one file (add `-I` flags for the ODBC, simdjson and curl headers when they
  are not on the default include path, e.g. `-I/opt/homebrew/include` with Homebrew):
  ```
  clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Isrc -Itests $(curl-config --cflags) <file>
  ```
  For files that do not include curl/simdjson/zstd/lz4 headers, a Windows syntax check is
  `x86_64-w64-mingw32-g++ -std=c++17 -fsyntax-only -Isrc -Itests <file>`; the full Windows cross
  build is `packaging/windows/cross_build_mingw.sh`.
* Unit tests use `tests/unit/test_framework.h` (`TEST_CASE`, `CHECK`, `CHECK_EQ`, `REQUIRE`,
  `CHECK_THROWS_AS`). Tests must be deterministic and need no network.

## Shared private interfaces (implemented by one module, used by others)

### src/util/tz.h (implemented in src/util/tz.cpp)
```cpp
namespace trino_odbc {
class TimeZoneDb {
 public:
  static TimeZoneDb& global();
  /// Offset (seconds east of UTC) in effect at the given UTC instant. False when the zone is unknown.
  bool utcOffsetAtUtc(std::string_view zoneName, int64_t utcEpochSeconds, int32_t* offsetSeconds);
  /// Offset for a wall-clock instant expressed as epoch seconds "as if UTC" (resolves gaps/overlaps by
  /// choosing the earlier offset). False when unknown.
  bool utcOffsetAtLocal(std::string_view zoneName, int64_t localEpochSeconds, int32_t* offsetSeconds);
  bool isKnown(std::string_view zoneName);
  /// IANA name of the OS zone ("" if unknown): $TZ, /etc/localtime symlink, /etc/timezone,
  /// Windows: GetDynamicTimeZoneInformation + Windows->IANA table.
  static std::string systemZoneName();
};
/// Parses "+05:30", "-0800", "Z", "UTC" into offset seconds. False otherwise.
bool parseFixedOffset(std::string_view text, int32_t* offsetSeconds);
}
```
Zone names "UTC", "Z", "GMT", "Etc/UTC", fixed offsets "+HH:MM" must always resolve without files; the default
session zone Europe/Istanbul (also Asia/Istanbul, Turkey) falls back to a fixed +03:00 (`fixedZoneFallback`) when no
TZif file, ICU zone or Windows registry rule knows it.

### src/convert/temporal.h (implemented in src/convert/temporal.cpp)
```cpp
namespace trino_odbc {
struct CivilDate { int32_t year = 1970; int month = 1; int day = 1; };
struct CivilTime { int hour = 0, minute = 0, second = 0; int64_t nanos = 0; int precision = 0; };
struct ParsedTimestamp {
  CivilDate date; CivilTime time;
  bool hasZone = false;          // "... America/New_York" or "... +05:30"
  bool zoneIsOffset = false;     // true when the zone is a fixed offset
  int32_t offsetSeconds = 0;     // valid when zoneIsOffset
  std::string zoneName;          // IANA name when !zoneIsOffset
};
bool parseTrinoDate(std::string_view s, CivilDate& out);                    // YYYY-MM-DD (year may be negative / >4 digits)
bool parseTrinoTime(std::string_view s, CivilTime& out, int32_t* offsetSeconds /*nullable*/, bool* hasOffset /*nullable*/); // HH:MM:SS[.f{1,12}][±HH:MM]
bool parseTrinoTimestamp(std::string_view s, ParsedTimestamp& out);         // YYYY-MM-DD HH:MM:SS[.f...][ zone]
int64_t daysFromCivil(int32_t y, int m, int d);                              // days since 1970-01-01
void civilFromDays(int64_t days, int32_t& y, int& m, int& d);
bool isValidDate(int32_t y, int m, int d);
/// Converts a zoned timestamp to UTC wall clock (uses TimeZoneDb). False when the zone is unknown (value untouched).
bool timestampToUtc(ParsedTimestamp& ts);
/// Converts a zoned timestamp to the wall clock of `zone` (session time zone). Ok / UnknownValueZone (untouched) /
/// UnknownTargetZone (converted to UTC instead).
ZoneConversion timestampToZone(ParsedTimestamp& ts, std::string_view zone);
/// time with time zone -> `zone` at its current offset; false (shifted to UTC) when `zone` is unknown.
bool timeToZone(CivilTime& t, int32_t offsetSeconds, std::string_view zone);
std::string formatDate(const CivilDate& d);                                   // YYYY-MM-DD
std::string formatTime(const CivilTime& t, int fractionDigits);              // HH:MM:SS[.fff]
std::string formatTimestamp(const CivilDate& d, const CivilTime& t, int fractionDigits);
bool parseTrinoIntervalYearMonth(std::string_view s, SQL_INTERVAL_STRUCT& out);  // "[-]Y-M"
bool parseTrinoIntervalDaySecond(std::string_view s, SQL_INTERVAL_STRUCT& out);  // "[-]D HH:MM:SS.fff"
std::string formatIntervalYearMonth(const SQL_INTERVAL_STRUCT& v);
std::string formatIntervalDaySecond(const SQL_INTERVAL_STRUCT& v);
}
```

### src/convert/numeric.h (implemented in src/convert/numeric.cpp)
```cpp
namespace trino_odbc {
/// Exact decimal: value = (negative ? -1 : 1) * digits * 10^-scale ; digits has no leading zeros ("0" for zero).
struct Decimal { bool negative = false; std::string digits = "0"; int scale = 0;
  bool isZero() const; std::string toString() const; /* canonical, e.g. "-12.340" keeps scale */ };
bool parseDecimal(std::string_view s, Decimal& out);          // "[+-]digits[.digits]" and "1E5"/"1.5e-3" forms
/// Rescales to `scale` digits; returns true if fractional digits were lost (rounding half away from zero when round=true, else truncate).
bool rescaleDecimal(Decimal& d, int scale, bool round);
/// -> SQL_NUMERIC_STRUCT with the requested precision/scale. Returns false on overflow (precision exceeded);
/// *fractionTruncated set when digits were dropped.
bool decimalToNumeric(const Decimal& d, SQLSMALLINT precision, SQLSMALLINT scale, SQL_NUMERIC_STRUCT& out, bool* fractionTruncated);
bool numericToDecimal(const SQL_NUMERIC_STRUCT& in, Decimal& out);
/// Integer conversion: false on overflow; *fractionTruncated when nonzero fraction dropped.
bool decimalToInt64(const Decimal& d, int64_t& out, bool* fractionTruncated);
bool decimalToUInt64(const Decimal& d, uint64_t& out, bool* fractionTruncated);
double decimalToDouble(const Decimal& d);
Decimal decimalFromInt64(int64_t v);
Decimal decimalFromDouble(double v);          // shortest round-trip representation
/// Shortest round-trip text for doubles/floats ("1.5", "1e+300", "NaN", "Infinity").
std::string doubleToString(double v);
std::string floatToString(float v);
}
```

## ODBC escape functions the driver supports (src/sql/escape.cpp translates them; src/odbc/api_info.cpp reports exactly these)
String: ASCII BIT_LENGTH CHAR CHAR_LENGTH CHARACTER_LENGTH CONCAT INSERT LCASE LEFT LENGTH LOCATE(2 and 3 args)
LTRIM OCTET_LENGTH POSITION REPEAT REPLACE RIGHT RTRIM SOUNDEX SPACE SUBSTRING UCASE.
Numeric: ABS ACOS ASIN ATAN ATAN2 CEILING COS COT DEGREES EXP FLOOR LOG LOG10 MOD PI POWER RADIANS RAND ROUND SIGN SIN SQRT TAN TRUNCATE.
Time/date: CURRENT_DATE CURRENT_TIME CURRENT_TIMESTAMP CURDATE CURTIME DAYNAME DAYOFMONTH DAYOFWEEK DAYOFYEAR EXTRACT HOUR MINUTE MONTH MONTHNAME NOW QUARTER SECOND TIMESTAMPADD TIMESTAMPDIFF WEEK YEAR.
System: DATABASE USER IFNULL. Convert: CONVERT(expr, SQL_xxx) -> CAST.
TIMESTAMPADD/DIFF intervals: SQL_TSI_FRAC_SECOND SECOND MINUTE HOUR DAY WEEK MONTH QUARTER YEAR.
Trino translations: LOG->ln, LOG10->log10, RAND->random(), COT(x)->(1/tan(x)), TRUNCATE(x[,n])->truncate,
CHAR->chr, ASCII(s)->codepoint(CAST(substr(s,1,1) AS varchar(1))), LEFT(s,n)->substr(s,1,n),
RIGHT(s,n)->substr(s, greatest(length(s)-(n)+1, 1)), LOCATE(a,b)->strpos(b,a),
LOCATE(a,b,s)->(CASE WHEN strpos(substr(b,s),a)=0 THEN 0 ELSE strpos(substr(b,s),a)+s-1 END),
REPEAT(s,n)->array_join(repeat(s,n),''), SPACE(n)->array_join(repeat(' ',n),''), OCTET_LENGTH->length(to_utf8(s)),
BIT_LENGTH->(length(to_utf8(s))*8), INSERT(s,st,len,s2)->concat(substr(s,1,st-1),s2,substr(s,st+len)),
POSITION(a IN b)->position(a IN b), CURDATE/CURRENT_DATE->current_date, CURTIME/CURRENT_TIME->localtime,
NOW/CURRENT_TIMESTAMP->localtimestamp, DAYNAME->date_format(x,'%W'), MONTHNAME->date_format(x,'%M'),
DAYOFMONTH->day, DAYOFWEEK->((day_of_week(x) % 7) + 1), DAYOFYEAR->day_of_year, WEEK->week,
TIMESTAMPADD(SQL_TSI_X,n,t)->date_add('unit',n,t), TIMESTAMPDIFF(SQL_TSI_X,a,b)->date_diff('unit',a,b)
(FRAC_SECOND->'millisecond'), DATABASE()->current_catalog, USER()->current_user, IFNULL->coalesce.
CONVERT type map: SQL_BIGINT->bigint, SQL_BINARY/VARBINARY/LONGVARBINARY->varbinary, SQL_BIT->boolean,
SQL_CHAR/VARCHAR/LONGVARCHAR/WCHAR/WVARCHAR/WLONGVARCHAR->varchar, SQL_DATE/TYPE_DATE->date,
SQL_DECIMAL/NUMERIC->decimal(38,10), SQL_DOUBLE/FLOAT->double, SQL_INTEGER->integer, SQL_REAL->real,
SQL_SMALLINT->smallint, SQL_TINYINT->tinyint, SQL_TIME/TYPE_TIME->time, SQL_TIMESTAMP/TYPE_TIMESTAMP->timestamp,
SQL_GUID->uuid, SQL_INTERVAL_YEAR_TO_MONTH->interval year to month, SQL_INTERVAL_DAY_TO_SECOND->interval day to second.
{d 'x'}->DATE 'x', {t 'x'}->TIME 'x', {ts 'x'}->TIMESTAMP 'x', {interval ...}->INTERVAL ..., {oj ...}->..., {escape 'c'}->ESCAPE 'c',
{call p(...)}->CALL p(...), {? = call ...} -> DriverError HYC00.

## Exported entry points (listed in src/platform/win/trinoodbc.def, src/platform/unix/exports.map and exports.macos; implemented in src/odbc/)
ODBC 3.8 (ANSI): SQLAllocHandle SQLFreeHandle SQLAllocEnv SQLAllocConnect SQLAllocStmt SQLFreeEnv SQLFreeConnect
SQLConnect SQLDriverConnect SQLBrowseConnect SQLDisconnect SQLEndTran SQLTransact SQLGetInfo SQLGetFunctions
SQLGetTypeInfo SQLSetEnvAttr SQLGetEnvAttr SQLSetConnectAttr SQLGetConnectAttr SQLSetConnectOption SQLGetConnectOption
SQLSetStmtAttr SQLGetStmtAttr SQLSetStmtOption SQLGetStmtOption SQLPrepare SQLExecute SQLExecDirect SQLNativeSql
SQLNumParams SQLDescribeParam SQLBindParameter SQLSetParam SQLParamOptions SQLParamData SQLPutData SQLCancel SQLCancelHandle
SQLRowCount SQLMoreResults SQLCloseCursor SQLFreeStmt SQLGetCursorName SQLSetCursorName SQLSetPos SQLBulkOperations
SQLBindCol SQLFetch SQLFetchScroll SQLExtendedFetch SQLGetData SQLNumResultCols SQLDescribeCol SQLColAttribute SQLColAttributes
SQLTables SQLColumns SQLPrimaryKeys SQLForeignKeys SQLStatistics SQLSpecialColumns SQLProcedures SQLProcedureColumns
SQLTablePrivileges SQLColumnPrivileges SQLGetDiagRec SQLGetDiagField SQLError SQLCopyDesc SQLGetDescField SQLSetDescField
SQLGetDescRec SQLSetDescRec SQLSetScrollOptions.
Unicode (api_unicode.cpp): SQLConnectW SQLDriverConnectW SQLBrowseConnectW SQLGetInfoW SQLGetTypeInfoW SQLSetConnectAttrW
SQLGetConnectAttrW SQLSetConnectOptionW SQLGetConnectOptionW SQLSetStmtAttrW SQLGetStmtAttrW SQLPrepareW SQLExecDirectW
SQLNativeSqlW SQLGetCursorNameW SQLSetCursorNameW SQLDescribeColW SQLColAttributeW SQLColAttributesW SQLTablesW SQLColumnsW
SQLPrimaryKeysW SQLForeignKeysW SQLStatisticsW SQLSpecialColumnsW SQLProceduresW SQLProcedureColumnsW SQLTablePrivilegesW
SQLColumnPrivilegesW SQLGetDiagRecW SQLGetDiagFieldW SQLErrorW SQLGetDescFieldW SQLSetDescFieldW SQLGetDescRecW SQLSetDescRecW.
Windows setup (platform/win/setup.cpp): ConfigDSN ConfigDSNW ConfigDriver ConfigDriverW.
Not exported (driver-manager functions): SQLDataSources, SQLDrivers, SQLCompleteAsync.
