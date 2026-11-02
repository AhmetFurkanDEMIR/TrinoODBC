// Parameter tests: SQLBindParameter with every common C type and the literal text the driver
// renders into `EXECUTE IMMEDIATE ... USING` (asserted through the mock's `literals` echo column),
// the explicit-prepare variant, server-typed parameters (DESCRIBE INPUT), data-at-execution with
// SQLParamData/SQLPutData, SQLNumParams/SQLDescribeParam, parameter arrays and binding errors.
#include "integration/odbc_loader.h"

#include <cstring>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// What the mock returns for a parametrised SELECT: the USING text as sent and the literal count.
struct Echo {
  std::string literals;
  int64_t count = -1;
};

/// Fetches the echo row of an executed statement and closes the cursor.
Echo readEcho(SQLHSTMT stmt) {
  Echo e;
  const std::vector<std::string> names = columnNames(stmt);
  REQUIRE_EQ(names.size(), static_cast<size_t>(2));
  CHECK_EQ(names[0], std::string("literals"));
  CHECK_EQ(names[1], std::string("count"));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt), SQL_HANDLE_STMT, stmt);
  e.literals = getString(stmt, 1, nullptr, 4096);
  e.count = getInt64(stmt, 2).value_or(-1);
  CHECK(Odbc.SQLFetch(stmt) == SQL_NO_DATA);
  ODBC_REQUIRE(Odbc.SQLCloseCursor(stmt), SQL_HANDLE_STMT, stmt);
  return e;
}

/// "SELECT ? AS p1, ? AS p2, ..." with `n` markers.
std::string selectWithMarkers(int n) {
  std::string sql = "SELECT ";
  for (int i = 1; i <= n; ++i) {
    if (i > 1) sql += ", ";
    sql += "? AS p" + std::to_string(i);
  }
  return sql;
}

SQLRETURN bind(SQLHSTMT stmt, SQLUSMALLINT n, SQLSMALLINT cType, SQLSMALLINT sqlType, SQLULEN size, SQLSMALLINT digits,
               void* value, SQLLEN bufLen, SQLLEN* ind) {
  return Odbc.SQLBindParameter(stmt, n, SQL_PARAM_INPUT, cType, sqlType, size, digits, value, bufLen, ind);
}

/// One parameter of the "all types" case: bound value + the literal the driver must send.
struct AllTypes {
  char text[8] = "abc";
  SQLLEN textInd = SQL_NTS;
  SQLINTEGER integer = 42;
  SQLLEN integerInd = 0;
  SQLDOUBLE dbl = 1.5;
  SQLLEN dblInd = 0;
  unsigned char bit = 1;
  SQLLEN bitInd = 0;
  SQL_DATE_STRUCT date{};
  SQLLEN dateInd = 0;
  SQL_TIMESTAMP_STRUCT ts{};
  SQLLEN tsInd = 0;
  SQL_NUMERIC_STRUCT num{};
  SQLLEN numInd = 0;
  unsigned char bytes[2] = {0x01, 0x02};
  SQLLEN bytesInd = 2;
  char nullText[4] = "x";
  SQLLEN nullInd = SQL_NULL_DATA;

  AllTypes() {
    date.year = 2024;
    date.month = 1;
    date.day = 2;
    ts.year = 2024;
    ts.month = 1;
    ts.day = 2;
    ts.hour = 3;
    ts.minute = 4;
    ts.second = 5;
    ts.fraction = 678000000;  // nanoseconds
    std::memset(&num, 0, sizeof num);
    num.precision = 10;
    num.scale = 2;
    num.sign = 1;      // positive
    num.val[0] = 0xD2;  // 1234 little-endian -> 12.34
    num.val[1] = 0x04;
  }

  void bindAll(SQLHSTMT stmt) {
    ODBC_REQUIRE(bind(stmt, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, text, sizeof text, &textInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 2, SQL_C_SLONG, SQL_INTEGER, 0, 0, &integer, 0, &integerInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 3, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &dbl, 0, &dblInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 4, SQL_C_BIT, SQL_BIT, 0, 0, &bit, 0, &bitInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 5, SQL_C_TYPE_DATE, SQL_TYPE_DATE, 0, 0, &date, 0, &dateInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 6, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 23, 3, &ts, 0, &tsInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 7, SQL_C_NUMERIC, SQL_DECIMAL, 10, 2, &num, 0, &numInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 8, SQL_C_BINARY, SQL_VARBINARY, 0, 0, bytes, sizeof bytes, &bytesInd), SQL_HANDLE_STMT, stmt);
    ODBC_REQUIRE(bind(stmt, 9, SQL_C_CHAR, SQL_VARCHAR, 0, 0, nullText, sizeof nullText, &nullInd), SQL_HANDLE_STMT, stmt);
  }

  static const char* expected() {
    return "'abc', 42, DOUBLE '1.5', true, DATE '2024-01-02', TIMESTAMP '2024-01-02 03:04:05.678', DECIMAL '12.34', "
           "X'0102', CAST(NULL AS varchar)";
  }
};

void runAllTypes(Session& s) {
  Stmt stmt(s.dbc);
  AllTypes v;
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(9)), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT count = 0;
  ODBC_CHECK(Odbc.SQLNumParams(stmt.h, &count), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(9));
  v.bindAll(stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const Echo e = readEcho(stmt.h);
  CHECK_EQ(e.literals, std::string(AllTypes::expected()));
  CHECK_EQ(e.count, static_cast<int64_t>(9));
  // Executing again re-reads the buffers.
  std::strcpy(v.text, "xyz");
  v.integer = -7;
  v.bit = 0;
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const Echo again = readEcho(stmt.h);
  CHECK(again.literals.rfind("'xyz', -7, DOUBLE '1.5', false, ", 0) == 0);
}

}  // namespace

TEST_CASE("params/literals_execute_immediate") {
  HARNESS_MOCK_ONLY();
  Session s;
  runAllTypes(s);
}

TEST_CASE("params/literals_explicit_prepare") {
  HARNESS_MOCK_ONLY();
  // ExplicitPrepare=1 ships the statement in X-Trino-Prepared-Statement and runs EXECUTE name USING.
  Session s("ExplicitPrepare=1");
  runAllTypes(s);
}

TEST_CASE("params/literals_text_escaping_and_unicode") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(3)), SQL_HANDLE_STMT, stmt.h);
  WStr wide("Türkçe 日本語 😀");
  SQLLEN wideInd = SQL_NTS;
  char quoted[] = "it's";
  SQLLEN quotedInd = SQL_NTS;
  char counted[] = "abcdef";
  SQLLEN countedInd = 3;  // byte count: only "abc"
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_WCHAR, SQL_WVARCHAR, 0, 0, wide.ptr(), 0, &wideInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_CHAR, SQL_VARCHAR, 0, 0, quoted, sizeof quoted, &quotedInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 3, SQL_C_CHAR, SQL_CHAR, 10, 0, counted, sizeof counted, &countedInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const Echo e = readEcho(stmt.h);
  CHECK_EQ(e.literals, std::string("'Türkçe 日本語 😀', 'it''s', 'abc'"));
  CHECK_EQ(e.count, static_cast<int64_t>(3));
  // Without an indicator pointer character data is NUL-terminated.
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(1)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, quoted, sizeof quoted, nullptr), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals, std::string("'it''s'"));
  // SQLSetParam (ODBC 2) binds the same way; the narrow SQLExecDirect path renders too.
  ODBC_REQUIRE(Odbc.SQLSetParam(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, quoted, &quotedInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirectA(stmt.h, selectWithMarkers(1)), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals, std::string("'it''s'"));
}

TEST_CASE("params/literals_numeric_and_temporal_variants") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(12)), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT small = 7;
  SQLSCHAR tiny = -3;
  SQLBIGINT big = 9007199254740993LL;
  SQLREAL real = 2.5f;
  SQL_TIME_STRUCT time{};
  time.hour = 13;
  time.minute = 14;
  time.second = 15;
  unsigned char bit = 0;
  char dateText[] = "2024-05-06";
  char decimalText[] = "12.5";
  SQLDOUBLE roundMe = 0.30000000000000004;
  char hexText[] = "0a";
  SQLINTEGER asText = 5;
  char nanText[] = "NaN";
  SQLLEN nts = SQL_NTS;
  SQLLEN zero = 0;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, &small, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_STINYINT, SQL_TINYINT, 0, 0, &tiny, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 3, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &big, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 4, SQL_C_FLOAT, SQL_REAL, 0, 0, &real, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 5, SQL_C_TYPE_TIME, SQL_TYPE_TIME, 0, 0, &time, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 6, SQL_C_BIT, SQL_BIT, 0, 0, &bit, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 7, SQL_C_CHAR, SQL_TYPE_DATE, 0, 0, dateText, sizeof dateText, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 8, SQL_C_CHAR, SQL_DECIMAL, 10, 2, decimalText, sizeof decimalText, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 9, SQL_C_DOUBLE, SQL_NUMERIC, 10, 2, &roundMe, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 10, SQL_C_CHAR, SQL_VARBINARY, 0, 0, hexText, sizeof hexText, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 11, SQL_C_SLONG, SQL_VARCHAR, 0, 0, &asText, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 12, SQL_C_CHAR, SQL_DOUBLE, 0, 0, nanText, sizeof nanText, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const Echo e = readEcho(stmt.h);
  CHECK_EQ(e.literals, std::string("SMALLINT '7', TINYINT '-3', 9007199254740993, REAL '2.5', TIME '13:14:15', false, "
                                   "DATE '2024-05-06', DECIMAL '12.50', DECIMAL '0.30', X'0A', '5', DOUBLE 'NaN'"));
  CHECK_EQ(e.count, static_cast<int64_t>(12));

  // Intervals and GUIDs.
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(3)), SQL_HANDLE_STMT, stmt.h);
  SQL_INTERVAL_STRUCT ds{};
  ds.interval_type = SQL_IS_DAY_TO_SECOND;
  ds.interval_sign = SQL_FALSE;
  ds.intval.day_second.day = 1;
  ds.intval.day_second.hour = 2;
  ds.intval.day_second.minute = 3;
  ds.intval.day_second.second = 4;
  ds.intval.day_second.fraction = 500000;  // default interval seconds precision 6 -> .500
  SQL_INTERVAL_STRUCT ym{};
  ym.interval_type = SQL_IS_YEAR_TO_MONTH;
  ym.interval_sign = SQL_TRUE;
  ym.intval.year_month.year = 1;
  ym.intval.year_month.month = 2;
  char uuidText[] = "12345678-1234-5678-1234-567812345678";
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_INTERVAL_DAY_TO_SECOND, SQL_INTERVAL_DAY_TO_SECOND, 0, 0, &ds, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_YEAR_TO_MONTH, 0, 0, &ym, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 3, SQL_C_CHAR, SQL_GUID, 0, 0, uuidText, sizeof uuidText, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals,
           std::string("INTERVAL '1 02:03:04.500' DAY TO SECOND, INTERVAL '-1-2' YEAR TO MONTH, "
                       "UUID '12345678-1234-5678-1234-567812345678'"));
}

TEST_CASE("params/server_typed_lookup_and_describe_param") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT * FROM big_table WHERE id = ?"), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT count = 0;
  ODBC_CHECK(Odbc.SQLNumParams(stmt.h, &count), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(1));
  // DESCRIBE INPUT: the mock types `id = ?` as bigint.
  SQLSMALLINT sqlType = 0, digits = -1, nullable = -1;
  SQLULEN size = 0;
  ODBC_REQUIRE(Odbc.SQLDescribeParam(stmt.h, 1, &sqlType, &size, &digits, &nullable), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(sqlType, static_cast<SQLSMALLINT>(SQL_BIGINT));
  CHECK_EQ(size, static_cast<SQLULEN>(19));
  CHECK_EQ(digits, static_cast<SQLSMALLINT>(0));
  CHECK_EQ(nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE_UNKNOWN));
  ODBC_CHECK_ERROR(Odbc.SQLDescribeParam(stmt.h, 0, &sqlType, &size, &digits, &nullable), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLDescribeParam(stmt.h, 2, &sqlType, &size, &digits, &nullable), SQL_HANDLE_STMT, stmt.h, "07009");
  // The IPD was auto-populated from the server description.
  SQLHDESC ipd = nullptr;
  SQLINTEGER len = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_PARAM_DESC, &ipd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT ipdType = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 1, SQL_DESC_CONCISE_TYPE, &ipdType, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(ipdType, static_cast<SQLSMALLINT>(SQL_BIGINT));
  // Text bound as VARCHAR still renders as a bigint literal because the server type wins.
  char five[] = "5";
  SQLLEN nts = SQL_NTS;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, five, sizeof five, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(5));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(5));
  CHECK_EQ(getString(stmt.h, 2), std::string("name_5"));
  CHECK_EQ(getDouble(stmt.h, 3).value_or(-1), 2.0);
  CHECK_EQ(getString(stmt.h, 4), std::string("2024-01-01 00:00:04.000"));
  CHECK_EQ(getString(stmt.h, 5), std::string("5.0000"));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // A non-numeric text cannot become a bigint: 22018 before anything reaches the server.
  char bad[] = "five";
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, bad, sizeof bad, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "22018");
  // A native integer works as well (SQL_C_SLONG bound as SQL_INTEGER, widened by the server type).
  SQLINTEGER id = 77;
  SQLLEN zero = 0;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &id, 0, &zero), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 2), std::string("name_77"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // A parameter the server cannot type is described as the default character type.
  Stmt other(s.dbc);
  ODBC_REQUIRE(prepare(other.h, selectWithMarkers(2)), SQL_HANDLE_STMT, other.h);
  ODBC_REQUIRE(Odbc.SQLDescribeParam(other.h, 2, &sqlType, &size, &digits, &nullable), SQL_HANDLE_STMT, other.h);
  CHECK_EQ(sqlType, static_cast<SQLSMALLINT>(SQL_WVARCHAR));
  CHECK_EQ(size, static_cast<SQLULEN>(65535));
  ODBC_CHECK(Odbc.SQLNumParams(other.h, &count), SQL_HANDLE_STMT, other.h);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(2));
  // Markers inside string literals do not count.
  ODBC_REQUIRE(prepare(other.h, "SELECT '?' AS q, ? AS p"), SQL_HANDLE_STMT, other.h);
  ODBC_CHECK(Odbc.SQLNumParams(other.h, &count), SQL_HANDLE_STMT, other.h);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(1));
}

TEST_CASE("params/data_at_exec_two_chunks") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(3)), SQL_HANDLE_STMT, stmt.h);
  int tokenA = 1, tokenC = 3;
  SQLLEN daeA = SQL_LEN_DATA_AT_EXEC(11);
  SQLLEN daeC = SQL_DATA_AT_EXEC;
  SQLINTEGER middle = 42;
  SQLLEN middleInd = 0;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, &tokenA, 0, &daeA), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_INTEGER, 0, 0, &middle, 0, &middleInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 3, SQL_C_BINARY, SQL_VARBINARY, 0, 0, &tokenC, 0, &daeC), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLExecute(stmt.h) == SQL_NEED_DATA);
  // While data is pending most calls are sequence errors.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLPutData(stmt.h, narrow("x"), 1), SQL_HANDLE_STMT, stmt.h, "HY010");  // no SQLParamData yet
  SQLPOINTER token = nullptr;
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  CHECK(token == &tokenA);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, narrow("hello "), 6), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, narrow("world"), SQL_NTS), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  CHECK(token == &tokenC);
  unsigned char part1[2] = {0xAB, 0xCD};
  unsigned char part2[1] = {0xEF};
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, part1, 2), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, part2, 1), SQL_HANDLE_STMT, stmt.h);
  const SQLRETURN rc = Odbc.SQLParamData(stmt.h, &token);  // all data in: executes
  ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
  const Echo e = readEcho(stmt.h);
  CHECK_EQ(e.literals, std::string("'hello world', 42, X'ABCDEF'"));
  CHECK_EQ(e.count, static_cast<int64_t>(3));

  // NULL through SQLPutData(SQL_NULL_DATA) and a zero-chunk (empty) value.
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(2)), SQL_HANDLE_STMT, stmt.h);
  int tokenN = 1, tokenE = 2;
  SQLLEN daeN = SQL_DATA_AT_EXEC;
  SQLLEN daeE = SQL_DATA_AT_EXEC;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, &tokenN, 0, &daeN), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_WCHAR, SQL_WVARCHAR, 0, 0, &tokenE, 0, &daeE), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLExecute(stmt.h) == SQL_NEED_DATA);
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  CHECK(token == &tokenN);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, nullptr, SQL_NULL_DATA), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  CHECK(token == &tokenE);
  ODBC_REQUIRE(Odbc.SQLParamData(stmt.h, &token), SQL_HANDLE_STMT, stmt.h);  // no SQLPutData: empty string
  CHECK_EQ(readEcho(stmt.h).literals, std::string("CAST(NULL AS varchar), ''"));

  // SQLCancel abandons a pending data-at-execution sequence; the statement is idle afterwards.
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(1)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, &tokenN, 0, &daeN), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLExecute(stmt.h) == SQL_NEED_DATA);
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, narrow("partial"), SQL_NTS), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCancel(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLParamData(stmt.h, &token), SQL_HANDLE_STMT, stmt.h, "HY010");
  char plain[] = "plain";
  SQLLEN nts = SQL_NTS;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, plain, sizeof plain, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals, std::string("'plain'"));
  // Fixed-size data cannot be sent in pieces (HY019).
  SQLINTEGER wholeInt = 9;
  SQLLEN daeInt = SQL_DATA_AT_EXEC;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &wholeInt, 0, &daeInt), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLExecute(stmt.h) == SQL_NEED_DATA);
  CHECK(Odbc.SQLParamData(stmt.h, &token) == SQL_NEED_DATA);
  ODBC_CHECK(Odbc.SQLPutData(stmt.h, &wholeInt, sizeof wholeInt), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLPutData(stmt.h, &wholeInt, sizeof wholeInt), SQL_HANDLE_STMT, stmt.h, "HY019");
  ODBC_REQUIRE(Odbc.SQLParamData(stmt.h, &token), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals, std::string("9"));
}

TEST_CASE("params/paramset_size_two_insert") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER values[2] = {1, 2};
  SQLLEN inds[2] = {0, 0};
  SQLULEN processed = 0;
  SQLUSMALLINT status[2] = {0xFFFF, 0xFFFF};
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAMSET_SIZE, attrVal(2), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAM_STATUS_PTR, status, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, values, 0, inds), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(prepare(stmt.h, "INSERT INTO t VALUES (?)"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(processed, static_cast<SQLULEN>(2));
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));
  CHECK_EQ(status[1], static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));
  SQLLEN rows = 0;
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(2));  // one row per parameter set (mock: updateCount 1 each)
  // SQL_PARAM_IGNORE skips a set.
  SQLUSMALLINT operations[2] = {SQL_PARAM_PROCEED, SQL_PARAM_IGNORE};
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAM_OPERATION_PTR, operations, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(processed, static_cast<SQLULEN>(2));
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));
  CHECK_EQ(status[1], static_cast<SQLUSMALLINT>(SQL_PARAM_UNUSED));
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(1));
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAM_OPERATION_PTR, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
  // A failing set reports SQL_PARAM_ERROR with the set number in SQL_DIAG_ROW_NUMBER.
  char texts[2][8] = {"1", "two"};
  SQLLEN textInds[2] = {SQL_NTS, SQL_NTS};
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_CHAR, SQL_INTEGER, 0, 0, texts, 8, textInds), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "22018");
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_PARAM_SUCCESS));
  CHECK_EQ(status[1], static_cast<SQLUSMALLINT>(SQL_PARAM_ERROR));
  CHECK_EQ(processed, static_cast<SQLULEN>(2));
  SQLLEN rowNumber = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK(Odbc.SQLGetDiagField(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_ROW_NUMBER, &rowNumber, sizeof rowNumber, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowNumber, static_cast<SQLLEN>(2));
  // Row-wise parameter binding: one struct per set.
  struct Set {
    SQLINTEGER v;
    SQLLEN ind;
  } sets[2] = {{10, 0}, {20, 0}};
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAM_BIND_TYPE, attrVal(sizeof(Set)), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &sets[0].v, 0, &sets[0].ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(processed, static_cast<SQLULEN>(2));
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rows, static_cast<SQLLEN>(2));
  // ODBC 2 SQLParamOptions sets the same two things.
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAM_BIND_TYPE, attrVal(SQL_PARAM_BIND_BY_COLUMN), 0), SQL_HANDLE_STMT, stmt.h);
  SQLULEN processed2 = 0;
  ODBC_CHECK(Odbc.SQLParamOptions(stmt.h, 1, &processed2), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_PARAMSET_SIZE), static_cast<SQLULEN>(1));
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, values, 0, inds), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(processed2, static_cast<SQLULEN>(1));
  ODBC_CHECK_ERROR(Odbc.SQLParamOptions(stmt.h, 0, nullptr), SQL_HANDLE_STMT, stmt.h, "HY024");
}

TEST_CASE("params/paramset_with_result_set_hyc00") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER values[2] = {1, 2};
  SQLLEN inds[2] = {0, 0};
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAMSET_SIZE, attrVal(2), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, values, 0, inds), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(1)), SQL_HANDLE_STMT, stmt.h);
  // SQL_PARAM_ARRAY_SELECTS = SQL_PAS_NO_SELECT.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HYC00");
  // Back to one set the same statement works.
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAMSET_SIZE, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(readEcho(stmt.h).literals, std::string("1"));
}

TEST_CASE("params/unbound_and_reset_params") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(2)), SQL_HANDLE_STMT, stmt.h);
  // Nothing bound: 07002 before any request is made.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "07002");
  SQLINTEGER one = 1;
  SQLLEN ind = 0;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &one, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "07002");  // only one of two
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_INTEGER, 0, 0, &one, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // SQL_RESET_PARAMS unbinds everything; the APD count drops to 0.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_RESET_PARAMS), SQL_HANDLE_STMT, stmt.h);
  SQLHDESC apd = nullptr;
  SQLINTEGER len = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_PARAM_DESC, &apd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT count = -1;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(apd, 0, SQL_DESC_COUNT, &count, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, apd);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(0));
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "07002");
  // SQLBindParameter writes the APD and IPD records.
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_DECIMAL, 12, 3, &one, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(apd, 0, SQL_DESC_COUNT, &count, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, apd);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(2));
  SQLSMALLINT concise = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(apd, 2, SQL_DESC_CONCISE_TYPE, &concise, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, apd);
  CHECK_EQ(concise, static_cast<SQLSMALLINT>(SQL_C_SLONG));
  SQLHDESC ipd = nullptr;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_PARAM_DESC, &ipd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 2, SQL_DESC_CONCISE_TYPE, &concise, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(concise, static_cast<SQLSMALLINT>(SQL_DECIMAL));
  SQLSMALLINT precision = 0, scale = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 2, SQL_DESC_PRECISION, &precision, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ipd);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 2, SQL_DESC_SCALE, &scale, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(precision, static_cast<SQLSMALLINT>(12));
  CHECK_EQ(scale, static_cast<SQLSMALLINT>(3));
  SQLSMALLINT paramType = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 2, SQL_DESC_PARAMETER_TYPE, &paramType, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(paramType, static_cast<SQLSMALLINT>(SQL_PARAM_INPUT));
}

TEST_CASE("params/bind_parameter_validation") {
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER v = 1;
  SQLLEN ind = 0;
  ODBC_CHECK_ERROR(bind(stmt.h, 0, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HY093");
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_OUTPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT_OUTPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 1, 99, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HY105");
  ODBC_CHECK_ERROR(bind(stmt.h, 1, 9999, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HY003");
  ODBC_CHECK_ERROR(bind(stmt.h, 1, SQL_C_SLONG, 9999, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HY004");
  ODBC_CHECK_ERROR(bind(stmt.h, 1, SQL_C_CHAR, SQL_VARCHAR, 0, 0, &v, -5, &ind), SQL_HANDLE_STMT, stmt.h, "HY090");
  ODBC_CHECK_ERROR(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, nullptr, 0, &ind), SQL_HANDLE_STMT, stmt.h, "HY009");
  // Conversion errors surface at execute time with the parameter number.
  ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(2)), SQL_HANDLE_STMT, stmt.h);
  char text[] = "abc";
  SQLLEN nts = SQL_NTS;
  SQLINTEGER big = 300;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_CHAR, SQL_INTEGER, 0, 0, text, sizeof text, &nts), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "22018");
  SQLINTEGER column = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK(Odbc.SQLGetDiagField(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_COLUMN_NUMBER, &column, sizeof column, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(column, static_cast<SQLINTEGER>(2));
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_TINYINT, 0, 0, &big, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "22003");
  SQL_DATE_STRUCT badDate{};
  badDate.year = 2024;
  badDate.month = 13;
  badDate.day = 1;
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_TYPE_DATE, SQL_TYPE_DATE, 0, 0, &badDate, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "22008");
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_TYPE_DATE, SQL_INTEGER, 0, 0, &badDate, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "07006");
  // SQL_DEFAULT_PARAM has no meaning for Trino.
  SQLLEN defaultInd = SQL_DEFAULT_PARAM;
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &defaultInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HYC00");
  // The statement stays prepared and executes once the binding is fixed.
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // SQL_C_DEFAULT reads the buffer as the SQL type's default C type.
  if (ctx().mock) {
    ODBC_REQUIRE(prepare(stmt.h, selectWithMarkers(1)), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_DEFAULT, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(readEcho(stmt.h).literals, std::string("1"));
  }
}

TEST_CASE("params/roundtrip_against_real_server") {
  if (ctx().mock) {
    skip("the mock echoes literals instead of evaluating them");
    return;
  }
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT ? AS a, ? AS b, ? AS c"), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER a = 42;
  SQLLEN aInd = 0;
  char b[] = "text";
  SQLLEN bInd = SQL_NTS;
  SQLDOUBLE c = 1.5;
  SQLLEN cInd = 0;
  ODBC_REQUIRE(bind(stmt.h, 1, SQL_C_SLONG, SQL_INTEGER, 0, 0, &a, 0, &aInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 2, SQL_C_CHAR, SQL_VARCHAR, 0, 0, b, sizeof b, &bInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(bind(stmt.h, 3, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &c, 0, &cInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(42));
  CHECK_EQ(getString(stmt.h, 2), std::string("text"));
  CHECK_EQ(getDouble(stmt.h, 3).value_or(0), 1.5);
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
}
