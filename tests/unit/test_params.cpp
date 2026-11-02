// Unit tests for convert/params: bound parameter -> Trino literal rendering.
#include "unit/test_framework.h"

#include "convert/numeric.h"
#include "convert/params.h"
#include "convert/temporal.h"
#include "types/trino_type.h"
#include "util/errors.h"
#include "util/unicode.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>

using namespace trino_odbc;

namespace {

ConvertOptions defaultOpts() {
  ConvertOptions o;
  o.ansi = AnsiEncoding::Utf8;
  return o;
}

ParamValue make(SQLSMALLINT cType, SQLSMALLINT sqlType, const void* data, SQLLEN length, SQLULEN columnSize = 0,
                SQLSMALLINT decimalDigits = 0) {
  ParamValue v;
  v.cType = cType;
  v.sqlType = sqlType;
  v.data = data;
  v.length = length;
  v.columnSize = columnSize;
  v.decimalDigits = decimalDigits;
  return v;
}

std::string render(const ParamValue& v, const char* serverType = nullptr) {
  std::optional<TrinoType> st;
  if (serverType) st = TrinoType::parse(serverType);
  return renderParameterLiteral(v, defaultOpts(), st);
}

/// Renders a SQL_C_CHAR value bound with the given SQL type.
std::string renderText(const char* text, SQLSMALLINT sqlType, SQLULEN columnSize = 0, SQLSMALLINT decimalDigits = 0,
                       const char* serverType = nullptr) {
  return render(make(SQL_C_CHAR, sqlType, text, SQL_NTS, columnSize, decimalDigits), serverType);
}

template <typename T>
std::string renderFixed(SQLSMALLINT cType, SQLSMALLINT sqlType, const T& value, const char* serverType = nullptr,
                        SQLULEN columnSize = 0, SQLSMALLINT decimalDigits = 0) {
  return render(make(cType, sqlType, &value, 0, columnSize, decimalDigits), serverType);
}

/// SQLSTATE raised by `fn`, or "OK" when it returned normally.
std::string stateOf(const std::function<void()>& fn) {
  try {
    fn();
    return "OK";
  } catch (const DriverError& e) {
    return e.sqlstate();
  }
}

#define CHECK_STATE(expr, state) CHECK_EQ(stateOf([&] { (void)(expr); }), std::string(state))

SQL_DATE_STRUCT date(int y, int m, int d) {
  SQL_DATE_STRUCT s{};
  s.year = static_cast<SQLSMALLINT>(y);
  s.month = static_cast<SQLUSMALLINT>(m);
  s.day = static_cast<SQLUSMALLINT>(d);
  return s;
}

SQL_TIME_STRUCT time(int h, int mi, int s) {
  SQL_TIME_STRUCT t{};
  t.hour = static_cast<SQLUSMALLINT>(h);
  t.minute = static_cast<SQLUSMALLINT>(mi);
  t.second = static_cast<SQLUSMALLINT>(s);
  return t;
}

SQL_TIMESTAMP_STRUCT timestamp(int y, int m, int d, int h, int mi, int s, SQLUINTEGER nanos) {
  SQL_TIMESTAMP_STRUCT t{};
  t.year = static_cast<SQLSMALLINT>(y);
  t.month = static_cast<SQLUSMALLINT>(m);
  t.day = static_cast<SQLUSMALLINT>(d);
  t.hour = static_cast<SQLUSMALLINT>(h);
  t.minute = static_cast<SQLUSMALLINT>(mi);
  t.second = static_cast<SQLUSMALLINT>(s);
  t.fraction = nanos;
  return t;
}

SQL_INTERVAL_STRUCT intervalYm(bool negative, SQLUINTEGER years, SQLUINTEGER months) {
  SQL_INTERVAL_STRUCT iv{};
  iv.interval_type = SQL_IS_YEAR_TO_MONTH;
  iv.interval_sign = negative ? SQL_TRUE : SQL_FALSE;
  iv.intval.year_month.year = years;
  iv.intval.year_month.month = months;
  return iv;
}

SQL_INTERVAL_STRUCT intervalDs(bool negative, SQLUINTEGER d, SQLUINTEGER h, SQLUINTEGER mi, SQLUINTEGER s,
                               SQLUINTEGER nanos) {
  SQL_INTERVAL_STRUCT iv{};
  iv.interval_type = SQL_IS_DAY_TO_SECOND;
  iv.interval_sign = negative ? SQL_TRUE : SQL_FALSE;
  iv.intval.day_second.day = d;
  iv.intval.day_second.hour = h;
  iv.intval.day_second.minute = mi;
  iv.intval.day_second.second = s;
  iv.intval.day_second.fraction = nanos;
  return iv;
}

SQL_NUMERIC_STRUCT numeric(const char* text, SQLSMALLINT precision, SQLSMALLINT scale) {
  Decimal d;
  REQUIRE(parseDecimal(text, d));
  SQL_NUMERIC_STRUCT n;
  REQUIRE(decimalToNumeric(d, precision, scale, n, nullptr));
  return n;
}

}  // namespace

TEST_CASE("params: character values are quoted UTF-8 literals") {
  CHECK_EQ(renderText("it's", SQL_VARCHAR), std::string("'it''s'"));
  CHECK_EQ(renderText("", SQL_VARCHAR), std::string("''"));
  CHECK_EQ(renderText("plain", SQL_CHAR), std::string("'plain'"));
  CHECK_EQ(renderText("x", SQL_WLONGVARCHAR), std::string("'x'"));
  // Explicit byte length wins over the terminator.
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_VARCHAR, "abcdef", 3)), std::string("'abc'"));
  // Unicode buffers: length is in bytes.
  const WString wide = utf8ToWide("h\xC3\xA9llo 'q'");
  CHECK_EQ(render(make(SQL_C_WCHAR, SQL_WVARCHAR, wide.c_str(), SQL_NTS)), std::string("'h\xC3\xA9llo ''q'''"));
  CHECK_EQ(render(make(SQL_C_WCHAR, SQL_WVARCHAR, wide.c_str(), static_cast<SQLLEN>(2 * sizeof(SQLWCHAR)))),
           std::string("'h\xC3\xA9'"));
  CHECK_STATE(render(make(SQL_C_WCHAR, SQL_WVARCHAR, wide.c_str(), static_cast<SQLLEN>(sizeof(SQLWCHAR) + 1))), "HY090");
  CHECK_STATE(render(make(SQL_C_CHAR, SQL_VARCHAR, "abc", -7)), "HY090");
  // Server type wins: varchar target renders the text even when the app bound an integer type.
  CHECK_EQ(renderText("42", SQL_INTEGER, 0, 0, "varchar"), std::string("'42'"));
  CHECK_EQ(renderText("42", SQL_INTEGER, 0, 0, "varchar(3)"), std::string("'42'"));
}

TEST_CASE("params: NULL and special indicators") {
  CHECK_EQ(render(make(SQL_C_SLONG, SQL_INTEGER, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS integer)"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS varchar)"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_NULL_DATA, 10)), std::string("CAST(NULL AS varchar(10))"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_DECIMAL, nullptr, SQL_NULL_DATA, 10, 2)), std::string("CAST(NULL AS decimal(10,2))"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_UNKNOWN_TYPE, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS varchar)"));
  CHECK_EQ(render(make(SQL_C_DOUBLE, SQL_UNKNOWN_TYPE, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS double)"));
  CHECK_EQ(render(make(SQL_C_SLONG, SQL_INTEGER, nullptr, SQL_NULL_DATA), "timestamp(6)"),
           std::string("CAST(NULL AS timestamp(6))"));
  // DESCRIBE INPUT "unknown" carries no type: fall back to the bound type.
  CHECK_EQ(render(make(SQL_C_SLONG, SQL_INTEGER, nullptr, SQL_NULL_DATA), "unknown"), std::string("CAST(NULL AS integer)"));
  ParamValue v = make(SQL_C_SLONG, SQL_INTEGER, nullptr, 0);
  v.isNull = true;
  CHECK_EQ(render(v), std::string("CAST(NULL AS integer)"));
  v.isNull = false;
  v.isDefault = true;
  CHECK_STATE(render(v), "HYC00");
  v.isDefault = false;
  CHECK_STATE(render(v), "HY009");  // null pointer without a NULL indicator
  CHECK_STATE(render(make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_DATA_AT_EXEC)), "HY010");
  CHECK_STATE(render(make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_LEN_DATA_AT_EXEC(12))), "HY010");
}

TEST_CASE("params: data-at-execution buffers are the value") {
  const std::string text = "it's";
  ParamValue v = make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_LEN_DATA_AT_EXEC(0));
  v.daeData = &text;
  CHECK_EQ(render(v), std::string("'it''s'"));

  const std::string bytes("\x00\xFF\x10", 3);
  v = make(SQL_C_BINARY, SQL_VARBINARY, nullptr, SQL_DATA_AT_EXEC);
  v.daeData = &bytes;
  CHECK_EQ(render(v), std::string("X'00FF10'"));

  const WString wide = utf8ToWide("w\xC3\xB6rld");
  const std::string wideBytes(reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(SQLWCHAR));
  v = make(SQL_C_WCHAR, SQL_WVARCHAR, nullptr, SQL_DATA_AT_EXEC);
  v.daeData = &wideBytes;
  CHECK_EQ(render(v), std::string("'w\xC3\xB6rld'"));

  const std::string odd("ab", 1);
  v = make(SQL_C_WCHAR, SQL_WVARCHAR, nullptr, SQL_DATA_AT_EXEC);
  v.daeData = &odd;
  CHECK_STATE(render(v), "HY090");

  // A fixed-size value can also arrive through SQLPutData.
  const SQLINTEGER n = 77;
  const std::string intBytes(reinterpret_cast<const char*>(&n), sizeof n);
  v = make(SQL_C_SLONG, SQL_INTEGER, nullptr, SQL_DATA_AT_EXEC);
  v.daeData = &intBytes;
  CHECK_EQ(render(v), std::string("77"));
  const std::string shortBytes("\x01", 1);
  v.daeData = &shortBytes;
  CHECK_STATE(render(v), "HY090");

  // Numeric target with text DAE data is validated like bound text.
  const std::string num = " 12.50 ";
  v = make(SQL_C_CHAR, SQL_DECIMAL, nullptr, SQL_DATA_AT_EXEC, 10, 2);
  v.daeData = &num;
  CHECK_EQ(render(v), std::string("DECIMAL '12.50'"));
}

TEST_CASE("params: integer targets") {
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 42), std::string("42"));
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, -42), std::string("-42"));
  CHECK_EQ(renderFixed<SQLBIGINT>(SQL_C_SBIGINT, SQL_BIGINT, std::numeric_limits<SQLBIGINT>::max()),
           std::string("9223372036854775807"));
  CHECK_EQ(renderFixed<SQLBIGINT>(SQL_C_SBIGINT, SQL_BIGINT, std::numeric_limits<SQLBIGINT>::min()),
           std::string("-9223372036854775808"));
  CHECK_EQ(renderFixed<SQLSMALLINT>(SQL_C_SSHORT, SQL_SMALLINT, 7), std::string("SMALLINT '7'"));
  CHECK_EQ(renderFixed<SQLSCHAR>(SQL_C_STINYINT, SQL_TINYINT, -5), std::string("TINYINT '-5'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_UTINYINT, SQL_TINYINT, 127), std::string("TINYINT '127'"));
  CHECK_STATE(renderFixed<SQLCHAR>(SQL_C_UTINYINT, SQL_TINYINT, 200), "22003");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_TINYINT, 300), "22003");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_SMALLINT, 40000), "22003");
  CHECK_STATE(renderFixed<SQLBIGINT>(SQL_C_SBIGINT, SQL_INTEGER, 5000000000LL), "22003");
  CHECK_EQ(renderFixed<SQLBIGINT>(SQL_C_SBIGINT, SQL_INTEGER, 2147483647LL), std::string("2147483647"));
  CHECK_EQ(renderFixed<SQLUINTEGER>(SQL_C_ULONG, SQL_BIGINT, 4294967295u), std::string("4294967295"));
  CHECK_STATE(renderFixed<SQLUBIGINT>(SQL_C_UBIGINT, SQL_BIGINT, std::numeric_limits<SQLUBIGINT>::max()), "22003");
  // Legacy C type names map to their signed forms.
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_LONG, SQL_INTEGER, 9), std::string("9"));
  CHECK_EQ(renderFixed<SQLSMALLINT>(SQL_C_SHORT, SQL_INTEGER, 9), std::string("9"));
  CHECK_EQ(renderFixed<SQLSCHAR>(SQL_C_TINYINT, SQL_INTEGER, 9), std::string("9"));

  // Character sources are validated as numeric literals.
  CHECK_EQ(renderText("123", SQL_INTEGER), std::string("123"));
  CHECK_EQ(renderText(" -123 ", SQL_INTEGER), std::string("-123"));
  CHECK_EQ(renderText("+12.000", SQL_INTEGER), std::string("12"));
  CHECK_EQ(renderText("1e3", SQL_INTEGER), std::string("1000"));
  // Appendix D: truncated fractional digits are 22001 for every source (review finding 46).
  CHECK_STATE(renderText("12.5", SQL_INTEGER), "22001");
  CHECK_STATE(renderText("abc", SQL_INTEGER), "22018");
  CHECK_STATE(renderText("", SQL_INTEGER), "22018");
  CHECK_STATE(renderText("12abc", SQL_INTEGER), "22018");
  CHECK_STATE(renderText("99999999999", SQL_INTEGER), "22003");

  // Floating sources must be integral.
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, 3.0), std::string("3"));
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, 3.5), "22001");
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, 1.5), "22001");
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, 1e30), "22003");  // whole digits lost
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, std::numeric_limits<double>::quiet_NaN()), "22003");
  CHECK_EQ(renderFixed<SQLREAL>(SQL_C_FLOAT, SQL_BIGINT, 16777216.0f), std::string("16777216"));
  const SQL_NUMERIC_STRUCT n = numeric("12.00", 10, 2);
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_INTEGER, n), std::string("12"));
  const SQL_NUMERIC_STRUCT n2 = numeric("12.01", 10, 2);
  CHECK_STATE(renderFixed(SQL_C_NUMERIC, SQL_INTEGER, n2), "22001");
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_INTEGER, 1), std::string("1"));

  // Server-described types drive the literal shape.
  CHECK_EQ(renderText("5", SQL_UNKNOWN_TYPE, 0, 0, "bigint"), std::string("5"));
  CHECK_EQ(renderText("5", SQL_VARCHAR, 0, 0, "smallint"), std::string("SMALLINT '5'"));
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_VARCHAR, 5, "tinyint"), std::string("TINYINT '5'"));
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 500, "tinyint"), "22003");

  // Only the C type is known: unsigned types widen so nothing is lost.
  CHECK_EQ(renderFixed<SQLUINTEGER>(SQL_C_ULONG, SQL_UNKNOWN_TYPE, 4294967295u), std::string("4294967295"));
  CHECK_EQ(renderFixed<SQLUBIGINT>(SQL_C_UBIGINT, SQL_UNKNOWN_TYPE, std::numeric_limits<SQLUBIGINT>::max()),
           std::string("DECIMAL '18446744073709551615'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_UTINYINT, SQL_UNKNOWN_TYPE, 255), std::string("SMALLINT '255'"));
  CHECK_EQ(renderFixed<SQLUSMALLINT>(SQL_C_USHORT, SQL_UNKNOWN_TYPE, 65535), std::string("65535"));
  // Structured sources cannot become integers.
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_INTEGER, date(2024, 1, 1)), "07006");
}

TEST_CASE("params: floating point targets") {
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, 1.5), std::string("DOUBLE '1.5'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_FLOAT, -0.1), std::string("DOUBLE '-0.1'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, 1e300), std::string("DOUBLE '1e+300'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, 100.0), std::string("DOUBLE '100'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, std::numeric_limits<double>::quiet_NaN()),
           std::string("DOUBLE 'NaN'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, std::numeric_limits<double>::infinity()),
           std::string("DOUBLE 'Infinity'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, -std::numeric_limits<double>::infinity()),
           std::string("DOUBLE '-Infinity'"));
  CHECK_EQ(renderFixed<SQLREAL>(SQL_C_FLOAT, SQL_REAL, 0.1f), std::string("REAL '0.1'"));
  CHECK_EQ(renderFixed<SQLREAL>(SQL_C_FLOAT, SQL_DOUBLE, 0.1f), std::string("DOUBLE '0.1'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_REAL, 0.25), std::string("REAL '0.25'"));
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_REAL, 1e300), "22003");
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_DOUBLE, 3), std::string("DOUBLE '3'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_DOUBLE, 1), std::string("DOUBLE '1'"));
  const SQL_NUMERIC_STRUCT n = numeric("-12.340", 10, 3);
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_DOUBLE, n), std::string("DOUBLE '-12.340'"));

  CHECK_EQ(renderText("1.25", SQL_DOUBLE), std::string("DOUBLE '1.25'"));
  CHECK_EQ(renderText("1e-3", SQL_DOUBLE), std::string("DOUBLE '0.001'"));
  CHECK_EQ(renderText("nan", SQL_DOUBLE), std::string("DOUBLE 'NaN'"));
  CHECK_EQ(renderText("-Infinity", SQL_REAL), std::string("REAL '-Infinity'"));
  CHECK_EQ(renderText("inf", SQL_DOUBLE), std::string("DOUBLE 'Infinity'"));
  CHECK_STATE(renderText("1.2.3", SQL_DOUBLE), "22018");
  CHECK_STATE(renderText("", SQL_DOUBLE), "22018");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIME, SQL_DOUBLE, time(1, 2, 3)), "07006");

  // Precedence: the server says double even though the app bound an integer.
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 3, "double"), std::string("DOUBLE '3'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_UNKNOWN_TYPE, 2.5, "real"), std::string("REAL '2.5'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_UNKNOWN_TYPE, 2.5), std::string("DOUBLE '2.5'"));
  CHECK_EQ(renderFixed<SQLREAL>(SQL_C_FLOAT, SQL_UNKNOWN_TYPE, 2.5f), std::string("REAL '2.5'"));
}

TEST_CASE("params: decimal targets") {
  const SQL_NUMERIC_STRUCT n = numeric("12.34", 10, 2);
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_DECIMAL, n, nullptr, 10, 2), std::string("DECIMAL '12.34'"));
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_NUMERIC, n, nullptr, 0, 0), std::string("DECIMAL '12.34'"));
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_UNKNOWN_TYPE, n), std::string("DECIMAL '12.34'"));
  const SQL_NUMERIC_STRUCT neg = numeric("-0.5", 5, 1);
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_DECIMAL, neg, nullptr, 5, 1), std::string("DECIMAL '-0.5'"));
  const SQL_NUMERIC_STRUCT big = numeric(std::string(38, '9').c_str(), 38, 0);
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_DECIMAL, big, nullptr, 38, 0), "DECIMAL '" + std::string(38, '9') + "'");

  // Bound precision/scale rescale (rounding) and bound overflow.
  CHECK_EQ(renderText("12.345", SQL_DECIMAL, 10, 2), std::string("DECIMAL '12.35'"));
  CHECK_EQ(renderText("12.3", SQL_DECIMAL, 10, 2), std::string("DECIMAL '12.30'"));
  CHECK_EQ(renderText("12.345", SQL_DECIMAL), std::string("DECIMAL '12.345'"));  // unconstrained keeps digits
  CHECK_EQ(renderText("1e2", SQL_DECIMAL), std::string("DECIMAL '100'"));
  CHECK_EQ(renderText(" 7 ", SQL_NUMERIC, 3, 0), std::string("DECIMAL '7'"));
  CHECK_STATE(renderText("12345678901", SQL_DECIMAL, 10, 2), "22003");
  CHECK_STATE(renderText("abc", SQL_DECIMAL, 10, 2), "22018");
  CHECK_STATE(renderText("", SQL_DECIMAL), "22018");
  CHECK_STATE(renderText(("1" + std::string(38, '0')).c_str(), SQL_DECIMAL), "22003");  // 39 digits exceed Trino's max
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DECIMAL, 0.1 + 0.2, nullptr, 10, 2), std::string("DECIMAL '0.30'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DECIMAL, 2.5), std::string("DECIMAL '2.5'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DECIMAL, 1e-40), "DECIMAL '0." + std::string(38, '0') + "'");
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DECIMAL, std::numeric_limits<double>::infinity()), "22003");
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_DECIMAL, 12, nullptr, 5, 3), std::string("DECIMAL '12.000'"));
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_DECIMAL, 123, nullptr, 5, 3), "22003");
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_DECIMAL, 1, nullptr, 3, 1), std::string("DECIMAL '1.0'"));

  // Server-described decimal(p,s).
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 12, "decimal(5,3)"), std::string("DECIMAL '12.000'"));
  CHECK_EQ(renderText("1.23456", SQL_VARCHAR, 0, 0, "decimal(10,2)"), std::string("DECIMAL '1.23'"));
  CHECK_EQ(renderText("1.235", SQL_VARCHAR, 0, 0, "decimal(10,2)"), std::string("DECIMAL '1.24'"));
  CHECK_STATE(renderText("1000", SQL_VARCHAR, 0, 0, "decimal(5,3)"), "22003");
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_DECIMAL, date(2024, 1, 1)), "07006");
}

TEST_CASE("params: boolean targets") {
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_BIT, 1), std::string("true"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_BIT, 0), std::string("false"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_UNKNOWN_TYPE, 1), std::string("true"));
  CHECK_EQ(renderText("yes", SQL_BIT), std::string("true"));
  CHECK_EQ(renderText("FALSE", SQL_BIT), std::string("false"));
  CHECK_EQ(renderText(" 1 ", SQL_BIT), std::string("true"));
  CHECK_STATE(renderText("maybe", SQL_BIT), "22018");
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_BIT, 1), std::string("true"));
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_BIT, 0), std::string("false"));
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_BIT, 2), "22003");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_BIT, -1), "22003");
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTEGER, 0.0, "boolean"), std::string("false"));
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_BIT, 0.5), "22003");
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_BIT, date(2024, 1, 1)), "07006");
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_BIT, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS boolean)"));
}

TEST_CASE("params: date targets") {
  CHECK_EQ(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_DATE, date(2024, 2, 29)), std::string("DATE '2024-02-29'"));
  CHECK_EQ(renderFixed(SQL_C_DATE, SQL_DATE, date(1999, 12, 31)), std::string("DATE '1999-12-31'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_DATE, SQL_UNKNOWN_TYPE, date(1, 1, 1)), std::string("DATE '0001-01-01'"));
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_DATE, date(2023, 2, 29)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_DATE, date(2023, 13, 1)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_DATE, date(0, 1, 1)), "22008");
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, timestamp(2024, 1, 5, 0, 0, 0, 0)), std::string("DATE '2024-01-05'"));
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, timestamp(2024, 1, 5, 0, 0, 1, 0)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, timestamp(2024, 1, 5, 0, 0, 0, 1)), "22008");
  CHECK_EQ(renderText("2024-01-05", SQL_TYPE_DATE), std::string("DATE '2024-01-05'"));
  CHECK_EQ(renderText(" 2024-1-5 ", SQL_TYPE_DATE), std::string("DATE '2024-01-05'"));
  CHECK_EQ(renderText("2024-01-05 00:00:00.000", SQL_TYPE_DATE), std::string("DATE '2024-01-05'"));
  CHECK_STATE(renderText("2024-01-05 10:00:00", SQL_TYPE_DATE), "22008");
  CHECK_STATE(renderText("2024/01/05", SQL_TYPE_DATE), "22018");
  CHECK_STATE(renderText("2024-02-30", SQL_TYPE_DATE), "22018");
  CHECK_STATE(renderText("10:00:00", SQL_TYPE_DATE), "22018");
  CHECK_STATE(renderText("", SQL_TYPE_DATE), "22018");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_TYPE_DATE, 5), "07006");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_DATE, time(1, 2, 3)), "07006");
  CHECK_EQ(renderText("2024-01-05", SQL_VARCHAR, 0, 0, "date"), std::string("DATE '2024-01-05'"));
}

TEST_CASE("params: time targets") {
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIME, time(10, 20, 30)), std::string("TIME '10:20:30'"));
  CHECK_EQ(renderFixed(SQL_C_TIME, SQL_TIME, time(0, 0, 0)), std::string("TIME '00:00:00'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIME, time(10, 20, 30), nullptr, 12, 3), std::string("TIME '10:20:30.000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIME, time(10, 20, 30), "time(6)"), std::string("TIME '10:20:30.000000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIME, SQL_UNKNOWN_TYPE, time(23, 59, 59)), std::string("TIME '23:59:59'"));
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIME, time(24, 0, 0)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIME, time(1, 60, 0)), "22008");
  // Timestamp sources keep their fraction (Trino TIME has one).
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIME, timestamp(2024, 1, 5, 10, 20, 30, 123456000)),
           std::string("TIME '10:20:30.123456'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIME, timestamp(2024, 1, 5, 10, 20, 30, 0), nullptr, 0, 3),
           std::string("TIME '10:20:30.000'"));
  CHECK_EQ(renderText("10:20:30.5", SQL_TYPE_TIME), std::string("TIME '10:20:30.5'"));
  CHECK_EQ(renderText("10:20:30.500", SQL_TYPE_TIME), std::string("TIME '10:20:30.5'"));
  CHECK_EQ(renderText("10:20:30.5", SQL_TYPE_TIME, 0, 3), std::string("TIME '10:20:30.500'"));
  CHECK_EQ(renderText("10:20", SQL_TYPE_TIME), std::string("TIME '10:20:00'"));
  CHECK_EQ(renderText("2024-01-05 10:20:30", SQL_TYPE_TIME), std::string("TIME '10:20:30'"));
  CHECK_EQ(renderText("10:20:30+05:30", SQL_VARCHAR, 0, 0, "time(3) with time zone"), std::string("TIME '10:20:30.000+05:30'"));
  CHECK_EQ(renderText("10:20:30 -0800", SQL_TYPE_TIME), std::string("TIME '10:20:30-08:00'"));
  CHECK_STATE(renderText("10:20:30 America/New_York", SQL_TYPE_TIME), "22018");
  CHECK_STATE(renderText("25:00:00", SQL_TYPE_TIME), "22018");
  CHECK_STATE(renderText("10:20:30.1234567890123", SQL_TYPE_TIME), "22018");
  CHECK_STATE(renderText("2024-01-05", SQL_TYPE_TIME), "22018");
  CHECK_STATE(renderText("noon", SQL_TYPE_TIME), "22018");
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_TIME, date(2024, 1, 5)), "07006");
}

TEST_CASE("params: timestamp targets") {
  const SQL_TIMESTAMP_STRUCT ts = timestamp(2024, 1, 5, 10, 20, 30, 0);
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, nullptr, 23, 3),
           std::string("TIMESTAMP '2024-01-05 10:20:30.000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, nullptr, 19, 0),
           std::string("TIMESTAMP '2024-01-05 10:20:30'"));
  CHECK_EQ(renderFixed(SQL_C_TIMESTAMP, SQL_TIMESTAMP, ts, nullptr, 19, 0), std::string("TIMESTAMP '2024-01-05 10:20:30'"));
  // Only the C type is known: default precision 3, growing to fit nonzero digits.
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_UNKNOWN_TYPE, ts), std::string("TIMESTAMP '2024-01-05 10:20:30.000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_UNKNOWN_TYPE, timestamp(2024, 1, 5, 10, 20, 30, 120000000)),
           std::string("TIMESTAMP '2024-01-05 10:20:30.120'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_UNKNOWN_TYPE, timestamp(2024, 1, 5, 10, 20, 30, 123456000)),
           std::string("TIMESTAMP '2024-01-05 10:20:30.123456'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, timestamp(2024, 1, 5, 10, 20, 30, 123456789), nullptr, 23, 3),
           std::string("TIMESTAMP '2024-01-05 10:20:30.123456789'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, timestamp(2024, 1, 5, 10, 20, 30, 1), nullptr, 23, 3),
           std::string("TIMESTAMP '2024-01-05 10:20:30.000000001'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, "timestamp(6)"),
           std::string("TIMESTAMP '2024-01-05 10:20:30.000000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, "timestamp(0)"),
           std::string("TIMESTAMP '2024-01-05 10:20:30'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, "timestamp(3) with time zone"),
           std::string("TIMESTAMP '2024-01-05 10:20:30.000'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, ts, "timestamp(12)"),
           std::string("TIMESTAMP '2024-01-05 10:20:30.000000000'"));  // struct carries at most nanos
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, timestamp(2024, 1, 5, 10, 20, 30, 1000000000)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, timestamp(2024, 2, 30, 10, 20, 30, 0)), "22008");
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, timestamp(2024, 1, 5, 10, 60, 30, 0)), "22008");
  CHECK_EQ(renderFixed(SQL_C_TYPE_DATE, SQL_TYPE_TIMESTAMP, date(2024, 1, 5), nullptr, 23, 3),
           std::string("TIMESTAMP '2024-01-05 00:00:00.000'"));
  // Time -> timestamp takes today's date and a zero fraction (Appendix D, review finding 47).
  {
    ConvertOptions utc;
    utc.sessionTimeZone = "UTC";
    const CivilDate today = currentCivilDate("UTC");
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", static_cast<int>(today.year), today.month, today.day);
    const SQL_TIME_STRUCT tm = time(10, 20, 30);
    ParamValue pv = make(SQL_C_TYPE_TIME, SQL_TYPE_TIMESTAMP, &tm, sizeof(tm));
    pv.columnSize = 23;
    pv.decimalDigits = 3;
    const std::string rendered = renderParameterLiteral(pv, utc, std::nullopt);
    // The date is re-read around midnight so the check never flakes on a day boundary.
    const CivilDate after = currentCivilDate("UTC");
    char day2[16];
    std::snprintf(day2, sizeof day2, "%04d-%02d-%02d", static_cast<int>(after.year), after.month, after.day);
    CHECK(rendered == "TIMESTAMP '" + std::string(day) + " 10:20:30.000'" ||
          rendered == "TIMESTAMP '" + std::string(day2) + " 10:20:30.000'");
    const std::string text = "10:20:30";
    ParamValue tv = make(SQL_C_CHAR, SQL_TYPE_TIMESTAMP, text.c_str(), SQL_NTS);
    tv.columnSize = 23;
    tv.decimalDigits = 3;
    const std::string renderedText = renderParameterLiteral(tv, utc, std::nullopt);
    CHECK(renderedText == "TIMESTAMP '" + std::string(day) + " 10:20:30.000'" ||
          renderedText == "TIMESTAMP '" + std::string(day2) + " 10:20:30.000'");
  }
  CHECK_STATE(renderFixed(SQL_C_TYPE_TIME, SQL_TYPE_TIMESTAMP, time(25, 2, 3)), "22008");
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_TYPE_TIMESTAMP, 1.0), "07006");

  // Text sources are normalised to Trino syntax.
  CHECK_EQ(renderText("2024-01-05 10:20:30", SQL_TYPE_TIMESTAMP, 23, 3), std::string("TIMESTAMP '2024-01-05 10:20:30.000'"));
  CHECK_EQ(renderText("2024-01-05T10:20:30.5Z", SQL_VARCHAR, 0, 0, "timestamp(3) with time zone"),
           std::string("TIMESTAMP '2024-01-05 10:20:30.500 UTC'"));
  CHECK_EQ(renderText("2024-01-05 10:20:30 America/New_York", SQL_TYPE_TIMESTAMP, 0, 0),
           std::string("TIMESTAMP '2024-01-05 10:20:30 America/New_York'"));
  CHECK_EQ(renderText("2024-01-05 10:20:30.123456789012 +05:30", SQL_TYPE_TIMESTAMP, 0, 0),
           std::string("TIMESTAMP '2024-01-05 10:20:30.123456789012+05:30'"));
  CHECK_EQ(renderText("2024-01-05", SQL_TYPE_TIMESTAMP, 23, 3), std::string("TIMESTAMP '2024-01-05 00:00:00.000'"));
  CHECK_EQ(renderText("2024-01-05", SQL_TYPE_TIMESTAMP, 19, 0), std::string("TIMESTAMP '2024-01-05 00:00:00'"));
  CHECK_EQ(renderText("2024-01-05 10:20", SQL_TYPE_TIMESTAMP, 19, 0), std::string("TIMESTAMP '2024-01-05 10:20:00'"));
  CHECK_STATE(renderText("2024-01-05 10:20:30 Mars/Olympus!", SQL_TYPE_TIMESTAMP), "22018");
  CHECK_STATE(renderText("2024-01-05 24:00:00", SQL_TYPE_TIMESTAMP), "22018");
  CHECK_STATE(renderText("yesterday", SQL_TYPE_TIMESTAMP), "22018");
}

TEST_CASE("params: binary targets") {
  const unsigned char bytes[] = {0xDE, 0xAD, 0x00, 0xBE, 0xEF};
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_VARBINARY, bytes, 5)), std::string("X'DEAD00BEEF'"));
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_LONGVARBINARY, bytes, 2)), std::string("X'DEAD'"));
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_BINARY, bytes, 0)), std::string("X''"));
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_UNKNOWN_TYPE, bytes, 1)), std::string("X'DE'"));
  ParamValue nts = make(SQL_C_BINARY, SQL_VARBINARY, bytes, SQL_NTS);
  CHECK_STATE(render(nts), "HY090");
  nts.bufferLength = 3;
  CHECK_EQ(render(nts), std::string("X'DEAD00'"));
  CHECK_EQ(renderText("dead", SQL_VARBINARY), std::string("X'DEAD'"));
  CHECK_EQ(renderText("", SQL_VARBINARY), std::string("X''"));
  CHECK_STATE(renderText("abc", SQL_VARBINARY), "22018");
  CHECK_STATE(renderText("xyzw", SQL_VARBINARY), "22018");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_VARBINARY, 1), "07006");
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_INTEGER, bytes, 2), "varbinary"), std::string("X'DEAD'"));
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_VARBINARY, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS varbinary)"));
  // Binary to character targets renders hex text.
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_VARCHAR, bytes, 2)), std::string("'DEAD'"));
  // SQL_C_DEFAULT follows the server type when nothing else was bound.
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_UNKNOWN_TYPE, bytes, 2), "varbinary"), std::string("X'DEAD'"));
}

TEST_CASE("params: interval targets") {
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_YEAR_TO_MONTH, intervalYm(false, 1, 2)),
           std::string("INTERVAL '1-2' YEAR TO MONTH"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_YEAR_TO_MONTH, intervalYm(true, 0, 14)),
           std::string("INTERVAL '-1-2' YEAR TO MONTH"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_MONTH, SQL_UNKNOWN_TYPE, intervalYm(false, 0, 7)), std::string("INTERVAL '0-7' YEAR TO MONTH"));
  // day_second.fraction is in the default interval seconds precision 6 (microseconds, review finding 18).
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_DAY_TO_SECOND, SQL_INTERVAL_DAY_TO_SECOND, intervalDs(false, 1, 2, 3, 4, 5000)),
           std::string("INTERVAL '1 02:03:04.005' DAY TO SECOND"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_SECOND, SQL_INTERVAL_SECOND, intervalDs(false, 0, 0, 0, 1, 500000)),
           std::string("INTERVAL '0 00:00:01.500' DAY TO SECOND"));
  CHECK_STATE(renderFixed(SQL_C_INTERVAL_SECOND, SQL_INTERVAL_SECOND, intervalDs(false, 0, 0, 0, 1, 1000000)), "22015");
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_DAY_TO_SECOND, SQL_INTERVAL_DAY_TO_SECOND, intervalDs(true, 0, 0, 0, 90, 0)),
           std::string("INTERVAL '-0 00:01:30' DAY TO SECOND"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_HOUR, SQL_INTERVAL_HOUR, intervalDs(false, 0, 30, 0, 0, 0)),
           std::string("INTERVAL '1 06:00:00' DAY TO SECOND"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_SECOND, SQL_UNKNOWN_TYPE, intervalDs(false, 0, 0, 0, 1, 999999)),
           std::string("INTERVAL '0 00:00:01.999' DAY TO SECOND"));
  CHECK_STATE(renderFixed(SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_INTERVAL_DAY_TO_SECOND, intervalYm(false, 1, 2)), "07006");
  CHECK_STATE(renderFixed(SQL_C_INTERVAL_DAY, SQL_INTERVAL_YEAR, intervalDs(false, 1, 0, 0, 0, 0)), "07006");
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_DAY_TO_SECOND, SQL_VARCHAR, intervalDs(false, 1, 2, 3, 4, 0), "interval day to second"),
           std::string("INTERVAL '1 02:03:04' DAY TO SECOND"));

  CHECK_EQ(renderText("3-4", SQL_INTERVAL_YEAR_TO_MONTH), std::string("INTERVAL '3-4' YEAR TO MONTH"));
  CHECK_EQ(renderText("-3-4", SQL_INTERVAL_YEAR_TO_MONTH), std::string("INTERVAL '-3-4' YEAR TO MONTH"));
  CHECK_EQ(renderText("1 02:03:04.5", SQL_INTERVAL_DAY_TO_SECOND), std::string("INTERVAL '1 02:03:04.5' DAY TO SECOND"));
  CHECK_EQ(renderText("5", SQL_INTERVAL_DAY), std::string("INTERVAL '5' DAY"));
  CHECK_EQ(renderText("5", SQL_INTERVAL_YEAR), std::string("INTERVAL '5' YEAR"));
  CHECK_STATE(renderText("5", SQL_INTERVAL_DAY_TO_SECOND), "22018");
  CHECK_STATE(renderText("1 02:99:00", SQL_INTERVAL_DAY_TO_SECOND), "22018");
  CHECK_STATE(renderText("1:2", SQL_INTERVAL_YEAR_TO_MONTH), "22018");
  CHECK_STATE(renderText("abc", SQL_INTERVAL_DAY_TO_SECOND), "22018");
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTERVAL_MONTH, 7), std::string("INTERVAL '7' MONTH"));
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTERVAL_SECOND, -90), std::string("INTERVAL '-90' SECOND"));
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTERVAL_DAY_TO_SECOND, 7), "07006");
  CHECK_STATE(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_INTERVAL_DAY, 1.5), "22003");
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_INTERVAL_YEAR, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS interval year to month)"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_INTERVAL_MINUTE_TO_SECOND, nullptr, SQL_NULL_DATA)),
           std::string("CAST(NULL AS interval day to second)"));
}

TEST_CASE("params: uuid targets") {
  SQLGUID g{};
  g.Data1 = 0x12345678;
  g.Data2 = 0x9abc;
  g.Data3 = 0xdef0;
  for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(i + 1);
  CHECK_EQ(renderFixed(SQL_C_GUID, SQL_GUID, g), std::string("UUID '12345678-9abc-def0-0102-030405060708'"));
  CHECK_EQ(renderFixed(SQL_C_GUID, SQL_UNKNOWN_TYPE, g), std::string("UUID '12345678-9abc-def0-0102-030405060708'"));
  CHECK_EQ(renderFixed(SQL_C_GUID, SQL_VARCHAR, g), std::string("'12345678-9abc-def0-0102-030405060708'"));
  CHECK_EQ(renderText("12345678-9ABC-DEF0-0102-030405060708", SQL_GUID), std::string("UUID '12345678-9abc-def0-0102-030405060708'"));
  CHECK_EQ(renderText("12345678-9abc-def0-0102-030405060708", SQL_VARCHAR, 0, 0, "uuid"),
           std::string("UUID '12345678-9abc-def0-0102-030405060708'"));
  CHECK_STATE(renderText("123456789abcdef00102030405060708", SQL_GUID), "22018");
  CHECK_STATE(renderText("not-a-uuid", SQL_GUID), "22018");
  const unsigned char raw[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  CHECK_EQ(render(make(SQL_C_BINARY, SQL_GUID, raw, 16)), std::string("UUID '12345678-9abc-def0-0102-030405060708'"));
  CHECK_STATE(render(make(SQL_C_BINARY, SQL_GUID, raw, 15)), "22018");
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_GUID, 1), "07006");
  CHECK_EQ(render(make(SQL_C_GUID, SQL_GUID, nullptr, SQL_NULL_DATA)), std::string("CAST(NULL AS uuid)"));
}

TEST_CASE("params: json, complex and exotic server types") {
  CHECK_EQ(renderText("{\"a\":1}", SQL_VARCHAR, 0, 0, "json"), std::string("JSON '{\"a\":1}'"));
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 5, "json"), std::string("JSON '5'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_BIT, 1, "json"), std::string("JSON 'true'"));
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_VARCHAR, date(2024, 1, 1), "json"), "07006");
  CHECK_EQ(renderText("[1,2]", SQL_VARCHAR, 0, 0, "array(integer)"), std::string("CAST(JSON '[1,2]' AS array(integer))"));
  CHECK_EQ(renderText("{\"k\":\"v\"}", SQL_WVARCHAR, 0, 0, "map(varchar, varchar)"),
           std::string("CAST(JSON '{\"k\":\"v\"}' AS map(varchar, varchar))"));
  CHECK_STATE(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_INTEGER, 1, "array(integer)"), "07006");
  CHECK_EQ(renderText("10.0.0.1", SQL_VARCHAR, 0, 0, "ipaddress"), std::string("CAST('10.0.0.1' AS ipaddress)"));
  // Type names are case-insensitive in Trino; the parser lowercases them.
  CHECK_EQ(renderText("x", SQL_VARCHAR, 0, 0, "HyperLogLog"), std::string("CAST('x' AS hyperloglog)"));
  CHECK_EQ(render(make(SQL_C_CHAR, SQL_VARCHAR, nullptr, SQL_NULL_DATA), "array(integer)"),
           std::string("CAST(NULL AS array(integer))"));
}

TEST_CASE("params: character targets from every source kind") {
  CHECK_EQ(renderFixed<SQLINTEGER>(SQL_C_SLONG, SQL_VARCHAR, 5), std::string("'5'"));
  CHECK_EQ(renderFixed<SQLBIGINT>(SQL_C_SBIGINT, SQL_WVARCHAR, -9000000000LL), std::string("'-9000000000'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_VARCHAR, 1.5), std::string("'1.5'"));
  CHECK_EQ(renderFixed<SQLDOUBLE>(SQL_C_DOUBLE, SQL_VARCHAR, 1e300), std::string("'1e+300'"));
  CHECK_EQ(renderFixed<SQLREAL>(SQL_C_FLOAT, SQL_VARCHAR, 0.1f), std::string("'0.1'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_VARCHAR, 1), std::string("'1'"));
  CHECK_EQ(renderFixed<SQLCHAR>(SQL_C_BIT, SQL_VARCHAR, 0), std::string("'0'"));
  CHECK_EQ(renderFixed(SQL_C_NUMERIC, SQL_VARCHAR, numeric("-12.340", 10, 3)), std::string("'-12.340'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_DATE, SQL_VARCHAR, date(2024, 1, 5)), std::string("'2024-01-05'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIME, SQL_VARCHAR, time(10, 20, 30)), std::string("'10:20:30'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_VARCHAR, timestamp(2024, 1, 5, 10, 20, 30, 500000000)),
           std::string("'2024-01-05 10:20:30.5'"));
  CHECK_EQ(renderFixed(SQL_C_TYPE_TIMESTAMP, SQL_VARCHAR, timestamp(2024, 1, 5, 10, 20, 30, 0)),
           std::string("'2024-01-05 10:20:30'"));
  CHECK_STATE(renderFixed(SQL_C_TYPE_DATE, SQL_VARCHAR, date(2024, 13, 5)), "22008");
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_VARCHAR, intervalYm(false, 1, 2)), std::string("'1-2'"));
  CHECK_EQ(renderFixed(SQL_C_INTERVAL_DAY_TO_SECOND, SQL_VARCHAR, intervalDs(true, 1, 2, 3, 4, 0)),
           std::string("'-1 02:03:04'"));
}

TEST_CASE("params: SQL_C_DEFAULT resolution and invalid C types") {
  const SQLINTEGER i = 12;
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_INTEGER, &i, 0)), std::string("12"));
  const SQLDOUBLE d = 2.5;
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_DOUBLE, &d, 0)), std::string("DOUBLE '2.5'"));
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_UNKNOWN_TYPE, "text", SQL_NTS)), std::string("'text'"));
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_UNKNOWN_TYPE, "7", SQL_NTS), "integer"), std::string("7"));
  const SQL_DATE_STRUCT dt = date(2024, 1, 5);
  CHECK_EQ(render(make(SQL_C_DEFAULT, SQL_TYPE_DATE, &dt, 0)), std::string("DATE '2024-01-05'"));
  CHECK_STATE(render(make(static_cast<SQLSMALLINT>(-999), SQL_INTEGER, &i, 0)), "HY003");
}
