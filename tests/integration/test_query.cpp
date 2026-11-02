// Query tests: simple selects, every types_demo column through SQLGetData (character, wide and
// native C types), NULL indicators, NaN/Infinity, zoned timestamps in the default session mode
// (Europe/Istanbul) and the utc/wallclock/string modes, the default session time zone, decimals to
// SQL_C_NUMERIC, varbinary to SQL_C_BINARY, unicode round trips, SQLDescribeCol/SQLColAttribute vs
// SQLColumns consistency, metadata before execute, row counts of DML/DDL, SQLMoreResults, USE,
// server warnings, and the query-timeout / cancel semantics of the execute phase.
#include "integration/odbc_loader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace harness;

namespace {

constexpr int kTypesDemoColumns = 26;
const char* const kUnicodeText = "Türkçe ğüşiöç 日本語 😀";

/// One types_demo column as the driver must describe it with the default connection options
/// (UseUnicodeSqlCharacterTypes=1, TimeZone=Europe/Istanbul, TimestampWithTimeZoneMode=session,
/// TimeWithTimeZoneMode=string, IntervalMode=string, DefaultStringColumnLength=65535).
struct ColumnExpectation {
  const char* name;
  SQLSMALLINT type;
  SQLULEN size;
  SQLSMALLINT digits;
};

const ColumnExpectation kTypesDemo[kTypesDemoColumns] = {
    {"c_boolean", SQL_BIT, 1, 0},
    {"c_tinyint", SQL_TINYINT, 3, 0},
    {"c_smallint", SQL_SMALLINT, 5, 0},
    {"c_integer", SQL_INTEGER, 10, 0},
    {"c_bigint", SQL_BIGINT, 19, 0},
    {"c_real", SQL_REAL, 7, 0},
    {"c_double", SQL_DOUBLE, 15, 0},
    {"c_decimal_10_2", SQL_DECIMAL, 10, 2},
    {"c_decimal_38_10", SQL_DECIMAL, 38, 10},
    {"c_varchar", SQL_WVARCHAR, 65535, 0},
    {"c_varchar20", SQL_WVARCHAR, 20, 0},
    {"c_char5", SQL_WCHAR, 5, 0},
    {"c_varbinary", SQL_VARBINARY, 65535, 0},
    {"c_json", SQL_WLONGVARCHAR, 65535, 0},
    {"c_date", SQL_TYPE_DATE, 10, 0},
    {"c_time3", SQL_TYPE_TIME, 12, 3},
    {"c_time6_tz", SQL_WVARCHAR, 21, 0},
    {"c_timestamp3", SQL_TYPE_TIMESTAMP, 23, 3},
    {"c_timestamp6_tz", SQL_TYPE_TIMESTAMP, 26, 6},
    {"c_interval_ym", SQL_WVARCHAR, 20, 0},
    {"c_interval_ds", SQL_WVARCHAR, 30, 0},
    {"c_array", SQL_WLONGVARCHAR, 65535, 0},
    {"c_map", SQL_WLONGVARCHAR, 65535, 0},
    {"c_row", SQL_WLONGVARCHAR, 65535, 0},
    {"c_uuid", SQL_WVARCHAR, 36, 0},
    {"c_ipaddress", SQL_WVARCHAR, 45, 0},
};

// Column 19 (c_timestamp6_tz) as text. Row 1 is "12:30 America/New_York" on a day that is already
// in EDT (UTC-4) = 16:30 UTC, row 3 is 02:30 at a fixed +02:00 offset = 00:30 UTC. The default
// session mode renders both in the default session zone Europe/Istanbul (UTC+03:00).
constexpr int kZonedTimestampColumn = 19;
const char* const kRow1ZonedSession = "2024-03-10 19:30:00.123456";   // Europe/Istanbul
const char* const kRow1ZonedUtc = "2024-03-10 16:30:00.123456";
const char* const kRow1ZonedBerlin = "2024-03-10 17:30:00.123456";    // CET, UTC+01:00
const char* const kRow1ZonedWallClock = "2024-03-10 12:30:00.123456";

const char* const kRow1Text[kTypesDemoColumns] = {
    "1", "127", "32767", "2147483647", "9223372036854775807", "1.5", "3.141592653589793", "12345.67",
    "1234567890123456789012345678.1234567890", "hello world", "twenty", "abc  ", "010203FF",
    "{\"a\":1,\"b\":[1,2]}", "2024-03-10", "12:34:56.789", "12:34:56.123456+05:30", "2024-03-10 02:30:00.123",
    kRow1ZonedSession, "1-2", "3 04:05:06.789", "[1,2,3]", "{\"a\":1,\"b\":2}", "[1,\"x\"]",
    "12345678-1234-5678-1234-567812345678", "10.0.0.1",
};

const char* const kRow3Text[kTypesDemoColumns] = {
    "0", "-128", "-32768", "-2147483648", "-9223372036854775808", "NaN", "-Infinity", "-0.01", "-1.0000000000",
    kUnicodeText, "Türkçe ğüşiöç", "xyz  ", "00", "\"str\"", "1900-01-01", "00:00:00.000", "23:59:59.999999-08:00",
    "1970-01-01 00:00:00.000", "2024-03-10 03:30:00.123456", "-0-11", "-1 00:00:00.000", "[]", "{}", "[null,\"y\"]",
    "00000000-0000-0000-0000-000000000000", "2001:db8::1",
};

/// Executes `SELECT * FROM types_demo` and positions the cursor on row `row` (1-based).
void openTypesDemoRow(SQLHSTMT stmt, int row) {
  ODBC_REQUIRE(execDirect(stmt, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, stmt);
  REQUIRE_EQ(numResultCols(stmt), static_cast<SQLSMALLINT>(kTypesDemoColumns));
  for (int i = 0; i < row; ++i) ODBC_REQUIRE(Odbc.SQLFetch(stmt), SQL_HANDLE_STMT, stmt);
}

/// True when the driver could not resolve the IANA zone of the row (no tz database on this host):
/// the value then comes back as wall-clock time together with a 01000 warning, which is the
/// documented fallback rather than a bug of the driver.
bool zoneUnknownWarning(SQLHSTMT stmt) {
  for (const DiagRec& r : diag(SQL_HANDLE_STMT, stmt)) {
    if (r.sqlstate == "01000" && r.message.find("unknown") != std::string::npos) return true;
  }
  return false;
}

/// Compares one text cell of types_demo, allowing the zone fallback for the zoned timestamp.
void checkTextCell(SQLHSTMT stmt, int col1, const std::string& actual, const char* expected, bool wide) {
  if (col1 == kZonedTimestampColumn && std::strcmp(expected, kRow1ZonedSession) == 0 && zoneUnknownWarning(stmt)) {
    info("America/New_York not resolvable on this host: accepting wall-clock value for column %d", col1);
    CHECK_EQ(actual, std::string(kRow1ZonedWallClock));
    return;
  }
  testfw::State::get().checks++;
  if (actual != expected) {
    testfw::fail(__FILE__, __LINE__,
                 std::string(wide ? "SQL_C_WCHAR" : "SQL_C_CHAR") + " column " + std::to_string(col1) + " (" +
                     kTypesDemo[col1 - 1].name + ") = \"" + actual + "\", expected \"" + expected + "\"",
                 false);
  }
}

/// Decimal text of a SQL_NUMERIC_STRUCT (little-endian 128-bit magnitude, scale, sign) so exact
/// values can be asserted without hard-coding byte arrays.
std::string numericToString(const SQL_NUMERIC_STRUCT& n) {
  // Big-endian working copy of the magnitude; repeated division by 10 yields the digits.
  std::vector<unsigned> be;
  for (int i = SQL_MAX_NUMERIC_LEN - 1; i >= 0; --i) be.push_back(n.val[i]);
  std::string digits;
  for (;;) {
    bool allZero = true;
    for (unsigned b : be) {
      if (b != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero) break;
    unsigned rem = 0;
    for (unsigned& b : be) {
      const unsigned cur = rem * 256u + b;
      b = cur / 10u;
      rem = cur % 10u;
    }
    digits.push_back(static_cast<char>('0' + rem));
  }
  if (digits.empty()) digits = "0";
  std::reverse(digits.begin(), digits.end());
  if (n.scale > 0) {
    const size_t scale = static_cast<size_t>(n.scale);
    if (digits.size() <= scale) digits.insert(0, scale + 1 - digits.size(), '0');
    digits.insert(digits.size() - scale, ".");
  }
  return (n.sign == 0 ? "-" : "") + digits;
}

template <typename T>
SQLRETURN getFixed(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT cType, T& out, SQLLEN& ind) {
  std::memset(&out, 0, sizeof(T));
  ind = 0;
  return Odbc.SQLGetData(stmt, col, cType, &out, static_cast<SQLLEN>(sizeof(T)), &ind);
}

}  // namespace

TEST_CASE("query/select_one") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(1));
  const ColumnDesc d = describeCol(stmt.h, 1);
  CHECK_EQ(d.name, std::string("_col0"));
  CHECK_EQ(d.type, static_cast<SQLSMALLINT>(SQL_INTEGER));
  CHECK_EQ(d.size, static_cast<SQLULEN>(10));
  CHECK_EQ(d.digits, static_cast<SQLSMALLINT>(0));
  CHECK_EQ(d.nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE));
  SQLLEN rowCount = 0;
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rowCount), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(1));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
  // After the last row the cursor is positioned after the result: no row to read from.
  SQLINTEGER v = 0;
  SQLLEN ind = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &v, sizeof(v), &ind), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("query/select_literal_string") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'hello'"), SQL_HANDLE_STMT, stmt.h);
  const ColumnDesc d = describeCol(stmt.h, 1);
  CHECK(d.type == SQL_WVARCHAR || d.type == SQL_VARCHAR || d.type == SQL_WCHAR || d.type == SQL_CHAR);
  CHECK_EQ(d.size, static_cast<SQLULEN>(5));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("hello"));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
}

TEST_CASE("query/select_version_mock") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT version()"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("483-mock"));
}

TEST_CASE("query/types_demo_describe_col") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, stmt.h);
  REQUIRE_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(kTypesDemoColumns));
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    const ColumnExpectation& e = kTypesDemo[i];
    const SQLUSMALLINT col = static_cast<SQLUSMALLINT>(i + 1);
    const ColumnDesc d = describeCol(stmt.h, col);
    testfw::State::get().checks++;
    if (d.name != e.name || d.type != e.type || d.size != e.size || d.digits != e.digits) {
      testfw::fail(__FILE__, __LINE__,
                   std::string("SQLDescribeColW(") + std::to_string(col) + ") = {" + d.name + ", " +
                       std::to_string(d.type) + ", " + std::to_string(d.size) + ", " + std::to_string(d.digits) +
                       "}, expected {" + e.name + ", " + std::to_string(e.type) + ", " + std::to_string(e.size) +
                       ", " + std::to_string(e.digits) + "}",
                   false);
    }
    CHECK_EQ(d.nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE));
    // SQLColAttribute must agree with SQLDescribeCol.
    CHECK_EQ(colAttrNum(stmt.h, col, SQL_DESC_CONCISE_TYPE), static_cast<SQLLEN>(e.type));
    CHECK_EQ(colAttrNum(stmt.h, col, SQL_DESC_LENGTH), static_cast<SQLLEN>(e.size));
    CHECK_EQ(colAttrNum(stmt.h, col, SQL_DESC_SCALE), static_cast<SQLLEN>(e.digits));
    CHECK_EQ(colAttrString(stmt.h, col, SQL_DESC_NAME), std::string(e.name));
    CHECK_EQ(colAttrString(stmt.h, col, SQL_DESC_LABEL), std::string(e.name));
  }
  CHECK_EQ(colAttrNum(stmt.h, 0, SQL_DESC_COUNT), static_cast<SQLLEN>(kTypesDemoColumns));
  // Verbose type / subcode of the temporal columns and a few type names.
  CHECK_EQ(colAttrNum(stmt.h, 15, SQL_DESC_TYPE), static_cast<SQLLEN>(SQL_DATETIME));
  CHECK_EQ(colAttrNum(stmt.h, 15, SQL_DESC_DATETIME_INTERVAL_CODE), static_cast<SQLLEN>(SQL_CODE_DATE));
  CHECK_EQ(colAttrNum(stmt.h, 16, SQL_DESC_DATETIME_INTERVAL_CODE), static_cast<SQLLEN>(SQL_CODE_TIME));
  CHECK_EQ(colAttrNum(stmt.h, 18, SQL_DESC_TYPE), static_cast<SQLLEN>(SQL_DATETIME));
  CHECK_EQ(colAttrNum(stmt.h, 18, SQL_DESC_DATETIME_INTERVAL_CODE), static_cast<SQLLEN>(SQL_CODE_TIMESTAMP));
  CHECK_EQ(colAttrNum(stmt.h, 18, SQL_DESC_PRECISION), static_cast<SQLLEN>(3));
  CHECK_EQ(colAttrString(stmt.h, 8, SQL_DESC_TYPE_NAME), std::string("decimal(10,2)"));
  CHECK_EQ(colAttrNum(stmt.h, 8, SQL_DESC_PRECISION), static_cast<SQLLEN>(10));
  CHECK_EQ(colAttrNum(stmt.h, 8, SQL_DESC_NUM_PREC_RADIX), static_cast<SQLLEN>(10));
  CHECK_EQ(colAttrString(stmt.h, 11, SQL_DESC_TYPE_NAME), std::string("varchar(20)"));
  CHECK_EQ(colAttrNum(stmt.h, 11, SQL_DESC_OCTET_LENGTH), static_cast<SQLLEN>(20 * sizeof(SQLWCHAR)));
  CHECK_EQ(colAttrNum(stmt.h, 11, SQL_DESC_CASE_SENSITIVE), static_cast<SQLLEN>(SQL_TRUE));
  CHECK_EQ(colAttrNum(stmt.h, 11, SQL_DESC_SEARCHABLE), static_cast<SQLLEN>(SQL_SEARCHABLE));
  CHECK_EQ(colAttrString(stmt.h, 19, SQL_DESC_TYPE_NAME), std::string("timestamp(6) with time zone"));
  CHECK_EQ(colAttrNum(stmt.h, 5, SQL_DESC_UNSIGNED), static_cast<SQLLEN>(SQL_FALSE));
  CHECK_EQ(colAttrNum(stmt.h, 10, SQL_DESC_UNSIGNED), static_cast<SQLLEN>(SQL_TRUE));
  CHECK_EQ(colAttrNum(stmt.h, 1, SQL_DESC_DISPLAY_SIZE), static_cast<SQLLEN>(1));
  CHECK_EQ(colAttrNum(stmt.h, 7, SQL_DESC_DISPLAY_SIZE), static_cast<SQLLEN>(24));
  CHECK_EQ(colAttrNum(stmt.h, 13, SQL_DESC_UPDATABLE), static_cast<SQLLEN>(SQL_ATTR_READONLY));
  // ODBC 2 attribute ids through SQLColAttributes.
  SQLLEN num = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK(Odbc.SQLColAttributes(stmt.h, 4, SQL_COLUMN_TYPE, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(num, static_cast<SQLLEN>(SQL_INTEGER));
  ODBC_CHECK(Odbc.SQLColAttributes(stmt.h, 4, SQL_COLUMN_PRECISION, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(num, static_cast<SQLLEN>(10));
  ODBC_CHECK(Odbc.SQLColAttributes(stmt.h, 11, SQL_COLUMN_LENGTH, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(num, static_cast<SQLLEN>(20 * sizeof(SQLWCHAR)));
  ODBC_CHECK(Odbc.SQLColAttributes(stmt.h, 0, SQL_COLUMN_COUNT, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(num, static_cast<SQLLEN>(kTypesDemoColumns));
  // Out-of-range column and unknown field.
  ODBC_CHECK_ERROR(Odbc.SQLColAttributeW(stmt.h, kTypesDemoColumns + 1, SQL_DESC_NAME, nullptr, 0, &len, &num),
                   SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLColAttributeW(stmt.h, 1, 32000, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h, "HY091");
}

TEST_CASE("query/types_demo_char_values") {
  HARNESS_MOCK_ONLY();
  Session s("AnsiEncoding=utf8");
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 1);
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    bool isNull = true;
    const std::string v = getString(stmt.h, static_cast<SQLUSMALLINT>(i + 1), &isNull);
    CHECK(!isNull);
    checkTextCell(stmt.h, i + 1, v, kRow1Text[i], false);
  }
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);  // row 2: NULLs (tested separately)
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);  // row 3
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    bool isNull = true;
    const std::string v = getString(stmt.h, static_cast<SQLUSMALLINT>(i + 1), &isNull);
    CHECK(!isNull);
    checkTextCell(stmt.h, i + 1, v, kRow3Text[i], false);
  }
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
}

TEST_CASE("query/types_demo_wchar_values") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 1);
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    bool isNull = true;
    // Small chunks so the wide chunk loop is exercised on every character and binary column.
    // Numeric and datetime values must fit whole (a too-small buffer is 22003, not 01004).
    const SQLSMALLINT t = kTypesDemo[i].type;
    const bool chunkable = t == SQL_WVARCHAR || t == SQL_WCHAR || t == SQL_WLONGVARCHAR || t == SQL_VARBINARY;
    const SQLLEN chunk = chunkable ? 8 * static_cast<SQLLEN>(sizeof(SQLWCHAR)) : 256;
    const std::string v = getWString(stmt.h, static_cast<SQLUSMALLINT>(i + 1), &isNull, chunk);
    CHECK(!isNull);
    checkTextCell(stmt.h, i + 1, v, kRow1Text[i], true);
  }
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    bool isNull = true;
    const std::string v = getWString(stmt.h, static_cast<SQLUSMALLINT>(i + 1), &isNull);
    CHECK(!isNull);
    checkTextCell(stmt.h, i + 1, v, kRow3Text[i], true);
  }
}

TEST_CASE("query/types_demo_null_row") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 2);
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    const SQLUSMALLINT col = static_cast<SQLUSMALLINT>(i + 1);
    char buf[64] = {0};
    SQLLEN ind = 0;
    ODBC_CHECK(Odbc.SQLGetData(stmt.h, col, SQL_C_CHAR, buf, sizeof(buf), &ind), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  }
  // Fixed-size targets report NULL through the indicator as well; a NULL without an indicator is 22002.
  SQLBIGINT big = 0;
  SQLLEN ind = 0;
  // Columns already retrieved report SQL_NO_DATA on a second SQLGetData; re-position the rowset
  // (SQL_GD_BLOCK) so the bookkeeping starts over before reading them again.
  ODBC_CHECK(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 5, SQL_C_SBIGINT, &big, sizeof(big), &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  SQL_TIMESTAMP_STRUCT ts;
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 18, SQL_C_TYPE_TIMESTAMP, &ts, sizeof(ts), &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 4, SQL_C_SLONG, &big, sizeof(SQLINTEGER), nullptr), SQL_HANDLE_STMT, stmt.h, "22002");
}

TEST_CASE("query/types_demo_native_types_row1") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 1);
  SQLLEN ind = 0;

  unsigned char bit = 0;
  ODBC_CHECK(getFixed(stmt.h, 1, SQL_C_BIT, bit, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(bit), 1);
  CHECK_EQ(ind, static_cast<SQLLEN>(1));
  SQLSCHAR tiny = 0;
  ODBC_CHECK(getFixed(stmt.h, 2, SQL_C_STINYINT, tiny, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(tiny), 127);
  SQLSMALLINT small = 0;
  ODBC_CHECK(getFixed(stmt.h, 3, SQL_C_SSHORT, small, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(small), 32767);
  SQLINTEGER integer = 0;
  ODBC_CHECK(getFixed(stmt.h, 4, SQL_C_SLONG, integer, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<long long>(integer), 2147483647LL);
  SQLBIGINT big = 0;
  ODBC_CHECK(getFixed(stmt.h, 5, SQL_C_SBIGINT, big, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<long long>(big), 9223372036854775807LL);
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQLBIGINT)));
  SQLREAL real = 0;
  ODBC_CHECK(getFixed(stmt.h, 6, SQL_C_FLOAT, real, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(real, 1.5f);
  SQLDOUBLE dbl = 0;
  ODBC_CHECK(getFixed(stmt.h, 7, SQL_C_DOUBLE, dbl, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(dbl, 3.141592653589793);

  // Decimals: precision/scale come from the column when the application does not set them.
  SQL_NUMERIC_STRUCT num;
  ODBC_CHECK(getFixed(stmt.h, 8, SQL_C_NUMERIC, num, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(num.precision), 10);
  CHECK_EQ(static_cast<int>(num.scale), 2);
  CHECK_EQ(static_cast<int>(num.sign), 1);
  CHECK_EQ(numericToString(num), std::string("12345.67"));
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQL_NUMERIC_STRUCT)));
  ODBC_CHECK(getFixed(stmt.h, 9, SQL_C_NUMERIC, num, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(num.precision), 38);
  CHECK_EQ(static_cast<int>(num.scale), 10);
  CHECK_EQ(numericToString(num), std::string("1234567890123456789012345678.1234567890"));

  const std::optional<std::vector<unsigned char>> bin = getBinary(stmt.h, 13);
  REQUIRE(bin.has_value());
  const std::vector<unsigned char> expectedBin = {1, 2, 3, 255};
  CHECK(*bin == expectedBin);

  SQL_DATE_STRUCT date;
  ODBC_CHECK(getFixed(stmt.h, 15, SQL_C_TYPE_DATE, date, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(date.year), 2024);
  CHECK_EQ(static_cast<int>(date.month), 3);
  CHECK_EQ(static_cast<int>(date.day), 10);
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)));

  // time(3) with a non-zero fraction into SQL_TIME_STRUCT: fraction dropped -> 01S07.
  SQL_TIME_STRUCT time;
  const SQLRETURN timeRc = getFixed(stmt.h, 16, SQL_C_TYPE_TIME, time, ind);
  CHECK(timeRc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01S07");
  CHECK_EQ(static_cast<int>(time.hour), 12);
  CHECK_EQ(static_cast<int>(time.minute), 34);
  CHECK_EQ(static_cast<int>(time.second), 56);

  SQL_TIMESTAMP_STRUCT ts;
  ODBC_CHECK(getFixed(stmt.h, 18, SQL_C_TYPE_TIMESTAMP, ts, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(ts.year), 2024);
  CHECK_EQ(static_cast<int>(ts.month), 3);
  CHECK_EQ(static_cast<int>(ts.day), 10);
  CHECK_EQ(static_cast<int>(ts.hour), 2);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<int>(ts.second), 0);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123000000UL);

  // timestamp(6) with time zone in the default session mode: 12:30 New York (EDT) -> 16:30 UTC
  // -> 19:30 Europe/Istanbul.
  const SQLRETURN tzRc = getFixed(stmt.h, 19, SQL_C_TYPE_TIMESTAMP, ts, ind);
  ODBC_CHECK(tzRc, SQL_HANDLE_STMT, stmt.h);
  const int expectedHour = (tzRc == SQL_SUCCESS_WITH_INFO && zoneUnknownWarning(stmt.h)) ? 12 : 19;
  if (expectedHour == 12) info("America/New_York not resolvable on this host: accepting wall-clock hour");
  CHECK_EQ(static_cast<int>(ts.hour), expectedHour);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123456000UL);

  // Intervals exposed as text still convert to SQL_INTERVAL_STRUCT on request.
  SQL_INTERVAL_STRUCT iv;
  ODBC_CHECK(getFixed(stmt.h, 20, SQL_C_INTERVAL_YEAR_TO_MONTH, iv, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_YEAR_TO_MONTH));
  CHECK_EQ(static_cast<int>(iv.interval_sign), static_cast<int>(SQL_FALSE));
  CHECK_EQ(static_cast<unsigned long>(iv.intval.year_month.year), 1UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.year_month.month), 2UL);
  ODBC_CHECK(getFixed(stmt.h, 21, SQL_C_INTERVAL_DAY_TO_SECOND, iv, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_DAY_TO_SECOND));
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.day), 3UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.hour), 4UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.minute), 5UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.second), 6UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.fraction), 789000UL);  // default precision 6

  SQLGUID guid;
  ODBC_CHECK(getFixed(stmt.h, 25, SQL_C_GUID, guid, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<unsigned long>(guid.Data1), 0x12345678UL);
  CHECK_EQ(static_cast<unsigned>(guid.Data2), 0x1234u);
  CHECK_EQ(static_cast<unsigned>(guid.Data3), 0x5678u);
  const unsigned char expectedData4[8] = {0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78};
  CHECK(std::memcmp(guid.Data4, expectedData4, 8) == 0);
}

TEST_CASE("query/types_demo_native_types_row3") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 3);
  SQLLEN ind = 0;
  unsigned char bit = 1;
  ODBC_CHECK(getFixed(stmt.h, 1, SQL_C_BIT, bit, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(bit), 0);
  SQLSCHAR tiny = 0;
  ODBC_CHECK(getFixed(stmt.h, 2, SQL_C_STINYINT, tiny, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(tiny), -128);
  SQLSMALLINT small = 0;
  ODBC_CHECK(getFixed(stmt.h, 3, SQL_C_SSHORT, small, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(small), -32768);
  SQLINTEGER integer = 0;
  ODBC_CHECK(getFixed(stmt.h, 4, SQL_C_SLONG, integer, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<long long>(integer), -2147483648LL);
  SQLBIGINT big = 0;
  ODBC_CHECK(getFixed(stmt.h, 5, SQL_C_SBIGINT, big, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK(big == (-9223372036854775807LL - 1));
  // NaN / -Infinity arrive as JSON strings and must become the IEEE values.
  SQLREAL real = 0;
  ODBC_CHECK(getFixed(stmt.h, 6, SQL_C_FLOAT, real, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK(std::isnan(real));
  SQLDOUBLE dbl = 0;
  ODBC_CHECK(getFixed(stmt.h, 7, SQL_C_DOUBLE, dbl, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK(std::isinf(dbl) && dbl < 0);
  SQL_NUMERIC_STRUCT num;
  ODBC_CHECK(getFixed(stmt.h, 8, SQL_C_NUMERIC, num, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(num.sign), 0);
  CHECK_EQ(numericToString(num), std::string("-0.01"));
  ODBC_CHECK(getFixed(stmt.h, 9, SQL_C_NUMERIC, num, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(numericToString(num), std::string("-1.0000000000"));
  const std::optional<std::vector<unsigned char>> bin = getBinary(stmt.h, 13);
  REQUIRE(bin.has_value());
  CHECK_EQ(bin->size(), static_cast<size_t>(1));
  CHECK_EQ(static_cast<int>((*bin)[0]), 0);
  SQL_DATE_STRUCT date;
  ODBC_CHECK(getFixed(stmt.h, 15, SQL_C_TYPE_DATE, date, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(date.year), 1900);
  CHECK_EQ(static_cast<int>(date.month), 1);
  CHECK_EQ(static_cast<int>(date.day), 1);
  SQL_TIME_STRUCT time;
  ODBC_CHECK(getFixed(stmt.h, 16, SQL_C_TYPE_TIME, time, ind), SQL_HANDLE_STMT, stmt.h);  // .000: no truncation
  CHECK_EQ(static_cast<int>(time.hour), 0);
  SQL_TIMESTAMP_STRUCT ts;
  ODBC_CHECK(getFixed(stmt.h, 18, SQL_C_TYPE_TIMESTAMP, ts, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(ts.year), 1970);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 0UL);
  // "+02:00" is a fixed offset and Europe/Istanbul always resolves: 00:30 UTC -> 03:30 Istanbul.
  ODBC_CHECK(getFixed(stmt.h, 19, SQL_C_TYPE_TIMESTAMP, ts, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(ts.day), 10);
  CHECK_EQ(static_cast<int>(ts.hour), 3);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123456000UL);
  SQL_INTERVAL_STRUCT iv;
  ODBC_CHECK(getFixed(stmt.h, 20, SQL_C_INTERVAL_YEAR_TO_MONTH, iv, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(iv.interval_sign), static_cast<int>(SQL_TRUE));
  CHECK_EQ(static_cast<unsigned long>(iv.intval.year_month.year), 0UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.year_month.month), 11UL);
  ODBC_CHECK(getFixed(stmt.h, 21, SQL_C_INTERVAL_DAY_TO_SECOND, iv, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(iv.interval_sign), static_cast<int>(SQL_TRUE));
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.day), 1UL);
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.fraction), 0UL);
  SQLGUID guid;
  ODBC_CHECK(getFixed(stmt.h, 25, SQL_C_GUID, guid, ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<unsigned long>(guid.Data1), 0UL);
}

TEST_CASE("query/timestamp_tz_modes") {
  HARNESS_MOCK_ONLY();
  // utc converts to UTC, session follows TimeZone, wallclock drops the zone, string keeps the
  // server text verbatim.
  {
    Session s("TimestampWithTimeZoneMode=utc");
    Stmt stmt(s.dbc);
    openTypesDemoRow(stmt.h, 1);
    CHECK_EQ(describeCol(stmt.h, kZonedTimestampColumn).type, static_cast<SQLSMALLINT>(SQL_TYPE_TIMESTAMP));
    const std::string v = getString(stmt.h, kZonedTimestampColumn);
    if (!zoneUnknownWarning(stmt.h)) CHECK_EQ(v, std::string(kRow1ZonedUtc));
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getString(stmt.h, kZonedTimestampColumn), std::string("2024-03-10 00:30:00.123456"));  // row 3
  }
  {
    Session s("TimeZone=Europe/Berlin");  // session mode (default) follows the configured zone
    Stmt stmt(s.dbc);
    openTypesDemoRow(stmt.h, 1);
    const std::string v = getString(stmt.h, kZonedTimestampColumn);
    if (!zoneUnknownWarning(stmt.h)) CHECK_EQ(v, std::string(kRow1ZonedBerlin));
  }
  {
    Session s("TimestampWithTimeZoneMode=session;TimeZone=+05:30");
    Stmt stmt(s.dbc);
    openTypesDemoRow(stmt.h, 3);
    CHECK_EQ(getString(stmt.h, kZonedTimestampColumn), std::string("2024-03-10 06:00:00.123456"));
  }
  {
    Session s("TimestampWithTimeZoneMode=wallclock");
    Stmt stmt(s.dbc);
    openTypesDemoRow(stmt.h, 1);
    CHECK_EQ(describeCol(stmt.h, kZonedTimestampColumn).type, static_cast<SQLSMALLINT>(SQL_TYPE_TIMESTAMP));
    CHECK_EQ(getString(stmt.h, kZonedTimestampColumn), std::string(kRow1ZonedWallClock));
  }
  {
    Session s("TimestampWithTimeZoneMode=string;TimeWithTimeZoneMode=utc");
    Stmt stmt(s.dbc);
    openTypesDemoRow(stmt.h, 1);
    const ColumnDesc d = describeCol(stmt.h, kZonedTimestampColumn);
    CHECK_EQ(d.type, static_cast<SQLSMALLINT>(SQL_WVARCHAR));
    CHECK_EQ(d.size, static_cast<SQLULEN>(26 + 1 + 40));
    CHECK_EQ(getString(stmt.h, kZonedTimestampColumn), std::string("2024-03-10 12:30:00.123456 America/New_York"));
    // time(6) with time zone in utc mode: 12:34:56.123456+05:30 -> 07:04:56.123456.
    CHECK_EQ(describeCol(stmt.h, 17).type, static_cast<SQLSMALLINT>(SQL_TYPE_TIME));
    CHECK_EQ(getString(stmt.h, 17), std::string("07:04:56.123456"));
  }
}

TEST_CASE("query/session_time_zone_default") {
  // Real Trino reports the session zone the driver sent (X-Trino-Time-Zone): Europe/Istanbul
  // unless TimeZone is set; TimeZone=system sends the OS zone.
  if (ctx().mock) {
    skip("requires a real Trino server (current_timezone())");
    return;
  }
  {
    Session s;
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_timezone()"), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getString(stmt.h, 1), std::string("Europe/Istanbul"));
  }
  {
    Session s("TimeZone=America/New_York");
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_timezone()"), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getString(stmt.h, 1), std::string("America/New_York"));
  }
  {
    // A zoned value computed on the server comes back in Istanbul wall-clock time.
    Session s;
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(execDirect(stmt.h, "SELECT TIMESTAMP '2024-03-10 12:30:00.123 America/New_York'"), SQL_HANDLE_STMT,
                 stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getString(stmt.h, 1), std::string("2024-03-10 19:30:00.123"));
  }
}

TEST_CASE("query/unicode_roundtrip") {
  Session s("AnsiEncoding=utf8");
  Stmt stmt(s.dbc);
  const std::string literal = std::string("SELECT '") + kUnicodeText + "'";
  ODBC_REQUIRE(execDirect(stmt.h, literal), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getWString(stmt.h, 1), std::string(kUnicodeText));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Narrow entry point + SQL_C_CHAR with AnsiEncoding=utf8: byte-exact UTF-8.
  ODBC_REQUIRE(execDirectA(stmt.h, literal), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string(kUnicodeText));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Bound SQL_C_WCHAR column: the indicator counts bytes, the text is NUL-terminated.
  std::vector<SQLWCHAR> wbuf(128, 0x7777);
  SQLLEN wind = 0;
  ODBC_REQUIRE(execDirect(stmt.h, literal), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 1, SQL_C_WCHAR, wbuf.data(), static_cast<SQLLEN>(wbuf.size() * sizeof(SQLWCHAR)), &wind),
             SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(wind, static_cast<SQLLEN>(wideLength(kUnicodeText) * static_cast<SQLLEN>(sizeof(SQLWCHAR))));
  CHECK_EQ(fromWide(wbuf.data()), std::string(kUnicodeText));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) {
    Stmt demo(s.dbc);
    openTypesDemoRow(demo.h, 3);
    CHECK_EQ(getWString(demo.h, 10), std::string(kUnicodeText));
    CHECK_EQ(getString(demo.h, 11), std::string("Türkçe ğüşiöç"));
  }
}

TEST_CASE("query/describe_col_matches_sqlcolumns") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt cols(s.dbc);
  WStr cat("mock");
  WStr schem("demo");
  WStr table("types_demo");
  ODBC_REQUIRE(Odbc.SQLColumnsW(cols.h, cat.ptr(), cat.len(), schem.ptr(), schem.len(), table.ptr(), table.len(), nullptr, 0),
               SQL_HANDLE_STMT, cols.h);
  const std::vector<std::vector<std::string>> rows = fetchAll(cols.h);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(kTypesDemoColumns));
  Stmt sel(s.dbc);
  ODBC_REQUIRE(execDirect(sel.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, sel.h);
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    const std::vector<std::string>& r = rows[static_cast<size_t>(i)];
    const ColumnDesc d = describeCol(sel.h, static_cast<SQLUSMALLINT>(i + 1));
    CHECK_EQ(r[3], d.name);                                  // COLUMN_NAME
    CHECK_EQ(r[4], std::to_string(d.type));                  // DATA_TYPE
    CHECK_EQ(r[6], std::to_string(d.size));                  // COLUMN_SIZE
    if (r[8] != "<NULL>") CHECK_EQ(r[8], std::to_string(d.digits));  // DECIMAL_DIGITS
    CHECK_EQ(r[5], colAttrString(sel.h, static_cast<SQLUSMALLINT>(i + 1), SQL_DESC_TYPE_NAME));  // TYPE_NAME
    CHECK_EQ(r[16], std::to_string(i + 1));                  // ORDINAL_POSITION
  }
}

TEST_CASE("query/prepare_describes_before_execute") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  // Metadata is available before SQLExecute (DESCRIBE OUTPUT through the prepared-statement header).
  CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(1));
  const ColumnDesc before = describeCol(stmt.h, 1);
  CHECK_EQ(before.name, std::string("_col0"));
  CHECK_EQ(before.type, static_cast<SQLSMALLINT>(SQL_INTEGER));
  SQLSMALLINT params = -1;
  ODBC_CHECK(Odbc.SQLNumParams(stmt.h, &params), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(params, static_cast<SQLSMALLINT>(0));
  // Prepared but not executed (state S2): SQLFetch is a function sequence error.
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const ColumnDesc after = describeCol(stmt.h, 1);
  CHECK_EQ(after.name, before.name);
  CHECK_EQ(after.type, before.type);
  CHECK_EQ(after.size, before.size);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  // Re-executing a prepared statement after closing the cursor works without preparing again.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));

  if (!ctx().mock) return;
  Stmt demo(s.dbc);
  ODBC_REQUIRE(prepare(demo.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, demo.h);
  REQUIRE_EQ(numResultCols(demo.h), static_cast<SQLSMALLINT>(kTypesDemoColumns));
  for (int i = 0; i < kTypesDemoColumns; ++i) {
    const ColumnDesc d = describeCol(demo.h, static_cast<SQLUSMALLINT>(i + 1));
    CHECK_EQ(d.name, std::string(kTypesDemo[i].name));
    CHECK_EQ(d.type, kTypesDemo[i].type);
    CHECK_EQ(d.size, kTypesDemo[i].size);
    CHECK_EQ(d.digits, kTypesDemo[i].digits);
  }
  // DESCRIBE OUTPUT also tells where the columns come from.
  CHECK_EQ(colAttrString(demo.h, 1, SQL_DESC_TABLE_NAME), std::string("types_demo"));
  CHECK_EQ(colAttrString(demo.h, 1, SQL_DESC_SCHEMA_NAME), std::string("demo"));
  CHECK_EQ(colAttrString(demo.h, 1, SQL_DESC_CATALOG_NAME), std::string("mock"));
  CHECK_EQ(colAttrString(demo.h, 1, SQL_DESC_BASE_COLUMN_NAME), std::string("c_boolean"));
  // A prepared INSERT describes as "no result set".
  Stmt ins(s.dbc);
  ODBC_REQUIRE(prepare(ins.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, ins.h);
  CHECK_EQ(numResultCols(ins.h), static_cast<SQLSMALLINT>(0));
  SQLSMALLINT len = 0;
  ODBC_CHECK_ERROR(Odbc.SQLDescribeColW(ins.h, 1, nullptr, 0, &len, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, ins.h, "07009");
}

TEST_CASE("query/rowcount_dml_ddl_and_more_results") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLLEN rows = 0;
  ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (1), (2), (3)"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(3));
  CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(0));
  // No cursor after DML: SQLFetch is a cursor-state error, SQLMoreResults just says "none".
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");
  CHECK(Odbc.SQLMoreResults(stmt.h) == SQL_NO_DATA);
  ODBC_REQUIRE(execDirect(stmt.h, "UPDATE t SET a = 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(3));
  ODBC_REQUIRE(execDirect(stmt.h, "DELETE FROM t"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(3));
  // DDL: no update count -> -1.
  ODBC_REQUIRE(execDirect(stmt.h, "CREATE TABLE t2 (a bigint)"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(-1));
  ODBC_REQUIRE(execDirect(stmt.h, "DROP TABLE t2"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(-1));
  // A result set: SQLMoreResults closes it and there is nothing after it.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLMoreResults(stmt.h) == SQL_NO_DATA);
  // SQLMoreResults returning SQL_NO_DATA puts an unprepared statement back into S1, where
  // SQLFetch is a function sequence error.
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  // The statement is reusable afterwards.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  // Read-only connections reject DML client-side.
  Session ro("ReadOnly=1");
  CHECK_EQ(getInfoString(ro.h(), SQL_DATA_SOURCE_READ_ONLY), std::string("Y"));
  Stmt rs(ro.dbc);
  ODBC_CHECK_ERROR(execDirect(rs.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, rs.h, "42000");
  ODBC_CHECK(execDirect(rs.h, "SELECT 1"), SQL_HANDLE_STMT, rs.h);
}

TEST_CASE("query/use_and_current_catalog") {
  HARNESS_MOCK_ONLY();
  Session s;
  CHECK_EQ(getInfoString(s.h(), SQL_DATABASE_NAME), std::string("mock"));
  Stmt stmt(s.dbc);
  // USE catalog.schema -> X-Trino-Set-Catalog / X-Trino-Set-Schema update the session.
  ODBC_REQUIRE(execDirect(stmt.h, "USE tpch.tiny"), SQL_HANDLE_STMT, stmt.h);
  std::vector<SQLWCHAR> buf(256);
  SQLINTEGER lenBytes = 0;
  ODBC_CHECK(Odbc.SQLGetConnectAttrW(s.h(), SQL_ATTR_CURRENT_CATALOG, buf.data(),
                                     static_cast<SQLINTEGER>(buf.size() * sizeof(SQLWCHAR)), &lenBytes),
             SQL_HANDLE_DBC, s.h());
  CHECK_EQ(fromWide(buf.data(), lenBytes / static_cast<SQLINTEGER>(sizeof(SQLWCHAR))), std::string("tpch"));
  CHECK_EQ(getInfoString(s.h(), SQL_DATABASE_NAME), std::string("tpch"));
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_catalog"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("tpch"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_schema"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("tiny"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // SQL_ATTR_CURRENT_CATALOG runs USE on the live connection.
  WStr mock("mock");
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_CURRENT_CATALOG, mock.ptr(),
                                     static_cast<SQLINTEGER>(mock.len() * static_cast<SQLSMALLINT>(sizeof(SQLWCHAR)))),
             SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLGetConnectAttrW(s.h(), SQL_ATTR_CURRENT_CATALOG, buf.data(),
                                     static_cast<SQLINTEGER>(buf.size() * sizeof(SQLWCHAR)), &lenBytes),
             SQL_HANDLE_DBC, s.h());
  CHECK_EQ(fromWide(buf.data(), lenBytes / static_cast<SQLINTEGER>(sizeof(SQLWCHAR))), std::string("mock"));
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_catalog"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("mock"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // ANSI setter with SQL_NTS, and the narrow ODBC 2 option wrapper.
  ODBC_CHECK(Odbc.SQLSetConnectAttr(s.h(), SQL_ATTR_CURRENT_CATALOG, narrow("tpch"), SQL_NTS), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getInfoString(s.h(), SQL_DATABASE_NAME), std::string("tpch"));
  ODBC_CHECK(Odbc.SQLSetConnectOption(s.h(), SQL_ATTR_CURRENT_CATALOG, reinterpret_cast<SQLULEN>(narrow("mock"))), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getInfoString(s.h(), SQL_DATABASE_NAME), std::string("mock"));
  // An unknown schema is reported by the server.
  ODBC_CHECK_ERROR(execDirect(stmt.h, "USE nosuchschema"), SQL_HANDLE_STMT, stmt.h, "3F000");
  // SET SESSION is a statement without a result set.
  ODBC_REQUIRE(execDirect(stmt.h, "SET SESSION query_max_run_time = '1h'"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(0));
}

TEST_CASE("query/warnings_success_with_info") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM warn_table");
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!d.empty());
  CHECK(hasState(d, "01000"));
  bool found = false;
  for (const DiagRec& r : d) {
    if (r.sqlstate == "01000" && r.message.find("mock warning") != std::string::npos) {
      found = true;
      CHECK_EQ(r.native, static_cast<SQLINTEGER>(1));
      CHECK(r.message.rfind("[Trino][ODBC Driver][Trino]", 0) == 0);
    }
  }
  CHECK(found);
  // SQL_DIAG_RETURNCODE mirrors the call's return code.
  SQLRETURN stored = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK(Odbc.SQLGetDiagField(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_RETURNCODE, &stored, sizeof(stored), &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(stored, static_cast<SQLRETURN>(SQL_SUCCESS_WITH_INFO));
  // The data is still there.
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  CHECK_EQ(getString(stmt.h, 2), std::string("warned"));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
}

TEST_CASE("query/comments_pass_through_to_server") {
  HARNESS_MOCK_ONLY();
  // The mock reads `rows=N` from a block comment: the driver must not strip comments.
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT count(*) FROM big_table /* rows=1234 */"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1234));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Line comments and a trailing semicolon are tolerated too.
  ODBC_REQUIRE(execDirect(stmt.h, "-- leading comment\nSELECT 1;"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("query/bound_column_and_getdata_on_same_row") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER bound = 0;
  SQLLEN ind = 0;
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SLONG, &bound, sizeof(bound), &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(bound, static_cast<SQLINTEGER>(1));
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQLINTEGER)));
  // SQL_GD_BOUND: SQLGetData works on a bound column, in any C type.
  CHECK_EQ(getString(stmt.h, 1), std::string("1"));
  // SQL_ARD_TYPE follows the binding; the value was already consumed above, so start a new row.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER viaArd = 0;
  SQLLEN ardInd = 0;
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_ARD_TYPE, &viaArd, sizeof(viaArd), &ardInd), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(viaArd, static_cast<SQLINTEGER>(1));
  // SQL_ATTR_RETRIEVE_DATA off: the bound buffer is left alone by SQLFetch.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_RETRIEVE_DATA, attrVal(SQL_RD_OFF), 0), SQL_HANDLE_STMT, stmt.h);
  bound = -5;
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(bound, static_cast<SQLINTEGER>(-5));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_RETRIEVE_DATA, attrVal(SQL_RD_ON), 0), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("query/decimal_as_double_option") {
  HARNESS_MOCK_ONLY();
  Session s("DecimalAsDouble=1");
  Stmt stmt(s.dbc);
  openTypesDemoRow(stmt.h, 1);
  const ColumnDesc d = describeCol(stmt.h, 8);
  CHECK_EQ(d.type, static_cast<SQLSMALLINT>(SQL_DOUBLE));
  CHECK_EQ(d.size, static_cast<SQLULEN>(15));
  CHECK_EQ(getDouble(stmt.h, 8).value_or(0.0), 12345.67);
  // The text form stays exact.
  CHECK_EQ(getString(stmt.h, 9), std::string("1234567890123456789012345678.1234567890"));
}

TEST_CASE("query/ansi_character_types_option") {
  HARNESS_MOCK_ONLY();
  Session s("UseUnicodeSqlCharacterTypes=0;UnboundedVarcharAsLongVarchar=1;UuidAsGuid=1;IntervalMode=interval");
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(describeCol(stmt.h, 10).type, static_cast<SQLSMALLINT>(SQL_LONGVARCHAR));  // unbounded varchar
  CHECK_EQ(describeCol(stmt.h, 11).type, static_cast<SQLSMALLINT>(SQL_VARCHAR));
  CHECK_EQ(describeCol(stmt.h, 12).type, static_cast<SQLSMALLINT>(SQL_CHAR));
  CHECK_EQ(describeCol(stmt.h, 13).type, static_cast<SQLSMALLINT>(SQL_LONGVARBINARY));
  CHECK_EQ(describeCol(stmt.h, 14).type, static_cast<SQLSMALLINT>(SQL_LONGVARCHAR));
  const ColumnDesc ym = describeCol(stmt.h, 20);
  CHECK_EQ(ym.type, static_cast<SQLSMALLINT>(SQL_INTERVAL_YEAR_TO_MONTH));
  CHECK_EQ(colAttrNum(stmt.h, 20, SQL_DESC_TYPE), static_cast<SQLLEN>(SQL_INTERVAL));
  CHECK_EQ(colAttrNum(stmt.h, 20, SQL_DESC_DATETIME_INTERVAL_CODE), static_cast<SQLLEN>(SQL_CODE_YEAR_TO_MONTH));
  const ColumnDesc ds = describeCol(stmt.h, 21);
  CHECK_EQ(ds.type, static_cast<SQLSMALLINT>(SQL_INTERVAL_DAY_TO_SECOND));
  CHECK_EQ(ds.digits, static_cast<SQLSMALLINT>(3));
  const ColumnDesc uuid = describeCol(stmt.h, 25);
  CHECK_EQ(uuid.type, static_cast<SQLSMALLINT>(SQL_GUID));
  CHECK_EQ(uuid.size, static_cast<SQLULEN>(36));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // SQL_C_DEFAULT follows the column's default C type: SQL_C_GUID for a GUID column.
  SQLGUID guid;
  SQLLEN ind = 0;
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 25, SQL_C_DEFAULT, &guid, sizeof(guid), &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQLGUID)));
  CHECK_EQ(static_cast<unsigned long>(guid.Data1), 0x12345678UL);
  // Interval column with SQL_C_DEFAULT -> SQL_INTERVAL_STRUCT; the fraction is expressed in the
  // ODBC default interval seconds precision of 6 digits (.789 -> 789000).
  SQL_INTERVAL_STRUCT iv;
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 21, SQL_C_DEFAULT, &iv, sizeof(iv), &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_DAY_TO_SECOND));
  CHECK_EQ(static_cast<unsigned long>(iv.intval.day_second.fraction), 789000UL);
  // Text of the interval is unchanged.
  CHECK_EQ(getString(stmt.h, 20), std::string("1-2"));
}

TEST_CASE("query/query_timeout_covers_only_execute") {
  HARNESS_MOCK_ONLY();
  // SQL_ATTR_QUERY_TIMEOUT bounds how long SQLExecDirect waits, not how long the application takes
  // to consume the rows: fetching after the timeout has elapsed must keep working, for the classic
  // protocol and for spooled segments alike. PrefetchPages=1 makes the prefetch thread issue most
  // of its requests after the pause.
  const char* const variants[] = {"PrefetchPages=1", "PrefetchPages=1;Spooling=0"};
  for (const char* extra : variants) {
    Session s(extra);
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLSetStmtAttr(stmt.h, SQL_ATTR_QUERY_TIMEOUT, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM big_table /* rows=20000 */"), SQL_HANDLE_STMT, stmt.h);
    sleepMs(1500);
    int64_t rows = 0;
    SQLRETURN rc;
    while ((rc = Odbc.SQLFetch(stmt.h)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) ++rows;
    CHECK_EQ(static_cast<int>(rc), static_cast<int>(SQL_NO_DATA));
    if (rc == SQL_ERROR) info("%s: fetch failed: %s", extra, diagText(SQL_HANDLE_STMT, stmt.h).c_str());
    CHECK_EQ(rows, static_cast<int64_t>(20000));
  }
}

TEST_CASE("query/statement_timeout_zero_overrides_dsn_query_timeout") {
  HARNESS_MOCK_ONLY();
  Session s("QueryTimeout=1");
  {
    // The DSN default applies to new statements ...
    Stmt stmt(s.dbc);
    CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_QUERY_TIMEOUT), static_cast<SQLULEN>(1));
    ODBC_CHECK_ERROR(execDirect(stmt.h, "SELECT * FROM slow_table"), SQL_HANDLE_STMT, stmt.h, "HYT00");
  }
  {
    // ... and an explicit 0 means "no timeout" (ODBC), not "use the DSN value".
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLSetStmtAttr(stmt.h, SQL_ATTR_QUERY_TIMEOUT, attrVal(0), 0), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_QUERY_TIMEOUT), static_cast<SQLULEN>(0));
    const double t0 = nowSeconds();
    ODBC_CHECK(execDirect(stmt.h, "SELECT * FROM slow_table"), SQL_HANDLE_STMT, stmt.h);
    CHECK(nowSeconds() - t0 > 1.5);  // slow_table stays queued for ~5 s
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  }
}

TEST_CASE("query/cancel_during_execute_is_never_reported_as_success") {
  HARNESS_MOCK_ONLY();
  // Cancels land at different points of the synchronous execute phase (POST, between polls, while a
  // poll is in flight). Whatever the timing, SQLExecDirect must fail with HY008: a cancel is never
  // mistaken for a finished query.
  Session s;
  const int delaysMs[] = {30, 120, 260, 510, 740};
  for (int i = 0; i < 5; ++i) {
    Stmt stmt(s.dbc);
    const bool viaConnection = (i % 2) == 1;
    std::thread canceller([&] {
      sleepMs(delaysMs[i]);
      if (viaConnection) Odbc.SQLCancelHandle(SQL_HANDLE_DBC, s.h());
      else Odbc.SQLCancel(stmt.h);
    });
    const double t0 = nowSeconds();
    const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM slow_table");
    const double elapsed = nowSeconds() - t0;
    canceller.join();
    ODBC_CHECK_ERROR(rc, SQL_HANDLE_STMT, stmt.h, "HY008");
    CHECK(elapsed < 4.0);
    // The statement is usable afterwards.
    ODBC_CHECK(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  }
}
