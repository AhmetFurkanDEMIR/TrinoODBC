// Unit tests for convert/convert.cpp: the SQL -> C conversion matrix over the five StorageKinds,
// SQLGetData chunking, SQL_C_NUMERIC precision/scale rules, temporal and interval targets.
#include "unit/test_framework.h"

#include "convert/convert.h"
#include "convert/numeric.h"
#include "convert/temporal.h"
#include "result/page.h"
#include "result/row_source.h"
#include "types/odbc_types.h"
#include "util/unicode.h"

#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace trino_odbc;

namespace {

bool haveZoneinfo() {
  std::FILE* f = std::fopen("/usr/share/zoneinfo/America/New_York", "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

ResultColumn makeCol(const char* trinoType, TypeMappingOptions tm = TypeMappingOptions{}) {
  return ResultColumn::make("c", trinoType, tm);
}

Column strCol(std::string_view s) {
  Column c(StorageKind::String);
  c.appendBytes(s);
  return c;
}
Column intCol(int64_t v) {
  Column c(StorageKind::Int64);
  c.appendInt64(v);
  return c;
}
Column dblCol(double v) {
  Column c(StorageKind::Double);
  c.appendDouble(v);
  return c;
}
Column boolCol(bool v) {
  Column c(StorageKind::Bool);
  c.appendBool(v);
  return c;
}
Column binCol(std::string_view bytes) {
  Column c(StorageKind::Binary);
  c.appendBytes(bytes);
  return c;
}
Column nullCol(StorageKind k) {
  Column c(k);
  c.appendNull();
  return c;
}

/// Drives convertToC against a local buffer and exposes the outcome.
struct Conv {
  Diag diag;
  ConvertOptions opts;
  SQLLEN ind = -99;
  alignas(16) unsigned char buf[512];
  size_t offset = 0;         // chunk state when `chunked`
  bool chunked = false;
  bool nullBuffer = false;
  bool nullIndicator = false;
  SQLSMALLINT numericPrecision = 0;
  SQLSMALLINT numericScale = 0;
  bool numericScaleSet = false;

  SQLRETURN run(const ResultColumn& col, const Column& data, SQLSMALLINT cType, SQLLEN bufLen = sizeof(buf)) {
    std::memset(buf, 0xEE, sizeof(buf));
    ind = -99;
    diag.clear();
    ConvertTarget t;
    t.cType = cType;
    t.buffer = nullBuffer ? nullptr : buf;
    t.bufferLength = bufLen;
    t.strLenOrInd = nullIndicator ? nullptr : &ind;
    t.numericPrecision = numericPrecision;
    t.numericScale = numericScale;
    t.numericScaleSet = numericScaleSet;
    return convertToC(col, data, 0, t, opts, chunked ? &offset : nullptr, diag, 1, 1);
  }
  std::string state() const { return diag.empty() ? std::string() : diag.records()[0].sqlstate; }
  bool hasState(const char* s) const {
    for (const auto& r : diag.records()) {
      if (r.sqlstate == s) return true;
    }
    return false;
  }
  template <typename T>
  T as() const {
    T v;
    std::memcpy(&v, buf, sizeof(T));
    return v;
  }
  std::string str() const { return std::string(reinterpret_cast<const char*>(buf)); }
  std::string wstr() const { return wideToUtf8(reinterpret_cast<const SQLWCHAR*>(buf), static_cast<SQLLEN>(SQL_NTS)); }
  std::string bytes(size_t n) const { return std::string(reinterpret_cast<const char*>(buf), n); }
};

/// One row of the exhaustive tables: convert and compare rc + first SQLSTATE.
struct Expect {
  SQLSMALLINT cType;
  SQLRETURN rc;
  const char* state;  // "" when none
};

void checkTable(Conv& cv, const ResultColumn& col, const Column& data, const std::vector<Expect>& rows,
                const char* label) {
  for (const Expect& e : rows) {
    const SQLRETURN rc = cv.run(col, data, e.cType);
    if (rc != e.rc || cv.state() != e.state) {
      std::string msg = std::string(label) + " -> " + cTypeName(e.cType) + ": got rc=" + std::to_string(rc) +
                        " state=" + cv.state() + " expected rc=" + std::to_string(e.rc) + " state=" + e.state;
      ::testfw::fail(__FILE__, __LINE__, msg, false);
      ::testfw::State::get().checks++;
    } else {
      ::testfw::State::get().checks++;
    }
  }
}

Decimal numericValue(const Conv& cv) {
  Decimal d;
  numericToDecimal(cv.as<SQL_NUMERIC_STRUCT>(), d);
  return d;
}

}  // namespace

// ---- Int64 storage --------------------------------------------------------------------------

TEST_CASE("convert: bigint to every integer width") {
  Conv cv;
  const ResultColumn col = makeCol("bigint");
  const Column v = intCol(100);
  CHECK_EQ(cv.run(col, v, SQL_C_STINYINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLSCHAR>()), 100);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLSCHAR)));
  CHECK_EQ(cv.run(col, v, SQL_C_UTINYINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLCHAR>()), 100);
  CHECK_EQ(cv.run(col, v, SQL_C_SSHORT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLSMALLINT>()), 100);
  CHECK_EQ(cv.run(col, v, SQL_C_USHORT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLUSMALLINT>()), 100);
  CHECK_EQ(cv.run(col, v, SQL_C_SLONG), SQL_SUCCESS);
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 100L);
  CHECK_EQ(cv.run(col, v, SQL_C_ULONG), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQLUINTEGER>()), 100UL);
  CHECK_EQ(cv.run(col, v, SQL_C_SBIGINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<long long>(cv.as<SQLBIGINT>()), 100LL);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(8));
  CHECK_EQ(cv.run(col, v, SQL_C_UBIGINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned long long>(cv.as<SQLUBIGINT>()), 100ULL);
  // Legacy signed aliases.
  CHECK_EQ(cv.run(col, v, SQL_C_TINYINT), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, v, SQL_C_SHORT), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, v, SQL_C_LONG), SQL_SUCCESS);
}

TEST_CASE("convert: bigint overflow per width is 22003") {
  Conv cv;
  const ResultColumn col = makeCol("bigint");
  checkTable(cv, col, intCol(128), {{SQL_C_STINYINT, SQL_ERROR, "22003"}, {SQL_C_UTINYINT, SQL_SUCCESS, ""}}, "128");
  checkTable(cv, col, intCol(256), {{SQL_C_UTINYINT, SQL_ERROR, "22003"}, {SQL_C_SSHORT, SQL_SUCCESS, ""}}, "256");
  checkTable(cv, col, intCol(-129), {{SQL_C_STINYINT, SQL_ERROR, "22003"}}, "-129");
  checkTable(cv, col, intCol(32768), {{SQL_C_SSHORT, SQL_ERROR, "22003"}, {SQL_C_USHORT, SQL_SUCCESS, ""}}, "32768");
  checkTable(cv, col, intCol(65536), {{SQL_C_USHORT, SQL_ERROR, "22003"}, {SQL_C_SLONG, SQL_SUCCESS, ""}}, "65536");
  checkTable(cv, col, intCol(2147483648LL),
             {{SQL_C_SLONG, SQL_ERROR, "22003"}, {SQL_C_ULONG, SQL_SUCCESS, ""}, {SQL_C_SBIGINT, SQL_SUCCESS, ""}},
             "2^31");
  checkTable(cv, col, intCol(4294967296LL), {{SQL_C_ULONG, SQL_ERROR, "22003"}}, "2^32");
  checkTable(cv, col, intCol(-1),
             {{SQL_C_UTINYINT, SQL_ERROR, "22003"},
              {SQL_C_USHORT, SQL_ERROR, "22003"},
              {SQL_C_ULONG, SQL_ERROR, "22003"},
              {SQL_C_UBIGINT, SQL_ERROR, "22003"},
              {SQL_C_STINYINT, SQL_SUCCESS, ""}},
             "-1");
  checkTable(cv, col, intCol(INT64_MIN), {{SQL_C_SBIGINT, SQL_SUCCESS, ""}, {SQL_C_UBIGINT, SQL_ERROR, "22003"}},
             "INT64_MIN");
}

TEST_CASE("convert: bigint to non-integer targets") {
  Conv cv;
  const ResultColumn col = makeCol("bigint");
  const Column v = intCol(-1234567890123LL);
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("-1234567890123"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(14));
  CHECK_EQ(cv.run(col, v, SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("-1234567890123"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(14 * sizeof(SQLWCHAR)));
  CHECK_EQ(cv.run(col, v, SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLDOUBLE>(), -1234567890123.0);
  CHECK_EQ(cv.run(col, v, SQL_C_FLOAT), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLREAL>(), static_cast<float>(-1234567890123.0));
  CHECK_EQ(cv.run(col, v, SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("-1234567890123"));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(8));
  CHECK_EQ(static_cast<long long>(cv.as<SQLBIGINT>()), -1234567890123LL);

  checkTable(cv, col, v,
             {{SQL_C_BIT, SQL_ERROR, "22003"},
              {SQL_C_TYPE_DATE, SQL_ERROR, "07006"},
              {SQL_C_TYPE_TIME, SQL_ERROR, "07006"},
              {SQL_C_TYPE_TIMESTAMP, SQL_ERROR, "07006"},
              {SQL_C_GUID, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_DAY_TO_SECOND, SQL_ERROR, "07006"}},
             "bigint");
  checkTable(cv, col, intCol(1), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "1");
  checkTable(cv, col, intCol(0), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "0");
  CHECK_EQ(cv.run(col, intCol(1), SQL_C_BIT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<unsigned char>()), 1);
}

TEST_CASE("convert: integer column native binary widths and SQL_C_DEFAULT") {
  Conv cv;
  CHECK_EQ(cv.run(makeCol("tinyint"), intCol(-5), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(1));
  CHECK_EQ(static_cast<int>(cv.as<SQLSCHAR>()), -5);
  CHECK_EQ(cv.run(makeCol("smallint"), intCol(-5), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(2));
  CHECK_EQ(cv.run(makeCol("integer"), intCol(-5), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(4));
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), -5L);

  CHECK_EQ(cv.run(makeCol("integer"), intCol(7), SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLINTEGER)));
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 7L);
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(7), SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLBIGINT)));
  CHECK_EQ(cv.run(makeCol("boolean"), boolCol(true), SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(1));
  CHECK_EQ(cv.run(makeCol("varchar"), strCol("hi"), SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(2 * sizeof(SQLWCHAR)));  // unicode default -> SQL_C_WCHAR
  CHECK_EQ(cv.wstr(), std::string("hi"));
  TypeMappingOptions ansi;
  ansi.unicode = false;
  CHECK_EQ(cv.run(makeCol("varchar", ansi), strCol("hi"), SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(2));
  CHECK_EQ(cv.str(), std::string("hi"));
}

// ---- Double storage -------------------------------------------------------------------------

TEST_CASE("convert: double to integers truncates with 01S07") {
  Conv cv;
  const ResultColumn col = makeCol("double");
  CHECK_EQ(cv.run(col, dblCol(3.75), SQL_C_SLONG), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 3L);
  CHECK_EQ(cv.run(col, dblCol(-3.75), SQL_C_SBIGINT), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<long long>(cv.as<SQLBIGINT>()), -3LL);
  CHECK_EQ(cv.run(col, dblCol(42.0), SQL_C_SSHORT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLSMALLINT>()), 42);
  CHECK_EQ(cv.run(col, dblCol(-0.5), SQL_C_UTINYINT), SQL_SUCCESS_WITH_INFO);  // truncates to 0
  CHECK_EQ(static_cast<int>(cv.as<SQLCHAR>()), 0);

  checkTable(cv, col, dblCol(1e19),
             {{SQL_C_SBIGINT, SQL_ERROR, "22003"}, {SQL_C_UBIGINT, SQL_SUCCESS, ""}, {SQL_C_SLONG, SQL_ERROR, "22003"}},
             "1e19");
  checkTable(cv, col, dblCol(2e19), {{SQL_C_UBIGINT, SQL_ERROR, "22003"}}, "2e19");
  checkTable(cv, col, dblCol(-1.0), {{SQL_C_ULONG, SQL_ERROR, "22003"}}, "-1.0");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  checkTable(cv, col, dblCol(nan),
             {{SQL_C_SLONG, SQL_ERROR, "22003"}, {SQL_C_BIT, SQL_ERROR, "22003"}, {SQL_C_NUMERIC, SQL_ERROR, "22003"},
              {SQL_C_DOUBLE, SQL_SUCCESS, ""}, {SQL_C_FLOAT, SQL_SUCCESS, ""}},
             "NaN");
  checkTable(cv, col, dblCol(inf), {{SQL_C_SBIGINT, SQL_ERROR, "22003"}, {SQL_C_FLOAT, SQL_SUCCESS, ""}}, "Inf");
}

TEST_CASE("convert: double to float range and bit rules") {
  Conv cv;
  const ResultColumn col = makeCol("double");
  CHECK_EQ(cv.run(col, dblCol(1.5), SQL_C_FLOAT), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLREAL>(), 1.5f);
  CHECK_EQ(cv.run(col, dblCol(1e300), SQL_C_FLOAT), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(col, dblCol(-1e300), SQL_C_FLOAT), SQL_ERROR);
  CHECK_EQ(cv.run(col, dblCol(1e300), SQL_C_DOUBLE), SQL_SUCCESS);

  checkTable(cv, col, dblCol(0.0), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "0.0");
  checkTable(cv, col, dblCol(1.0), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "1.0");
  checkTable(cv, col, dblCol(1.5), {{SQL_C_BIT, SQL_SUCCESS_WITH_INFO, "01S07"}}, "1.5");
  checkTable(cv, col, dblCol(0.25), {{SQL_C_BIT, SQL_SUCCESS_WITH_INFO, "01S07"}}, "0.25");
  checkTable(cv, col, dblCol(2.0), {{SQL_C_BIT, SQL_ERROR, "22003"}}, "2.0");
  checkTable(cv, col, dblCol(-0.5), {{SQL_C_BIT, SQL_ERROR, "22003"}}, "-0.5");
  CHECK_EQ(cv.run(col, dblCol(1.5), SQL_C_BIT), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<int>(cv.as<unsigned char>()), 1);
}

TEST_CASE("convert: double to text uses shortest round trip; real uses float text") {
  Conv cv;
  CHECK_EQ(cv.run(makeCol("double"), dblCol(0.1), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("0.1"));
  CHECK_EQ(cv.run(makeCol("double"), dblCol(-2.5), SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("-2.5"));
  CHECK_EQ(cv.run(makeCol("double"), dblCol(std::numeric_limits<double>::quiet_NaN()), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("NaN"));
  CHECK_EQ(cv.run(makeCol("double"), dblCol(std::numeric_limits<double>::infinity()), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("Infinity"));
  CHECK_EQ(cv.run(makeCol("double"), dblCol(-std::numeric_limits<double>::infinity()), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("-Infinity"));
  // 0.1f widened to double is 0.10000000149011612; the real column must print "0.1".
  CHECK_EQ(cv.run(makeCol("real"), dblCol(static_cast<double>(0.1f)), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("0.1"));
  CHECK_EQ(cv.run(makeCol("real"), dblCol(1.5), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLREAL)));
  CHECK_EQ(cv.as<SQLREAL>(), 1.5f);
  CHECK_EQ(cv.run(makeCol("double"), dblCol(1.5), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLDOUBLE)));
  CHECK_EQ(cv.as<SQLDOUBLE>(), 1.5);
  checkTable(cv, makeCol("double"), dblCol(1.5),
             {{SQL_C_TYPE_TIMESTAMP, SQL_ERROR, "07006"}, {SQL_C_GUID, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_SECOND, SQL_ERROR, "07006"}},
             "double");
}

TEST_CASE("convert: double to SQL_C_NUMERIC uses ARD scale, default scale 0 with 01S07") {
  Conv cv;
  const ResultColumn col = makeCol("double");
  CHECK_EQ(cv.run(col, dblCol(12.5), SQL_C_NUMERIC), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(numericValue(cv).toString(), std::string("12"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().precision), 38);
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().scale), 0);

  cv.numericPrecision = 10;
  cv.numericScale = 2;
  cv.numericScaleSet = true;
  CHECK_EQ(cv.run(col, dblCol(12.5), SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("12.50"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().precision), 10);
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().scale), 2);
  CHECK_EQ(cv.run(col, dblCol(-12.567), SQL_C_NUMERIC), SQL_SUCCESS_WITH_INFO);  // truncated, not rounded
  CHECK_EQ(numericValue(cv).toString(), std::string("-12.56"));
  CHECK_EQ(cv.run(col, dblCol(123456789.0), SQL_C_NUMERIC), SQL_ERROR);  // 11 digits > precision 10
  CHECK_EQ(cv.state(), std::string("22003"));
}

// ---- Bool storage ---------------------------------------------------------------------------

TEST_CASE("convert: boolean matrix") {
  Conv cv;
  const ResultColumn col = makeCol("boolean");
  CHECK_EQ(cv.run(col, boolCol(true), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("1"));
  CHECK_EQ(cv.run(col, boolCol(false), SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("0"));
  CHECK_EQ(cv.run(col, boolCol(true), SQL_C_BIT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<unsigned char>()), 1);
  CHECK_EQ(cv.run(col, boolCol(true), SQL_C_SLONG), SQL_SUCCESS);
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 1L);
  CHECK_EQ(cv.run(col, boolCol(false), SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLDOUBLE>(), 0.0);
  CHECK_EQ(cv.run(col, boolCol(true), SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("1"));
  CHECK_EQ(cv.run(col, boolCol(true), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(1));
  CHECK_EQ(static_cast<int>(cv.buf[0]), 1);
  checkTable(cv, col, boolCol(true),
             {{SQL_C_TYPE_DATE, SQL_ERROR, "07006"}, {SQL_C_GUID, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_YEAR, SQL_ERROR, "07006"}, {SQL_C_UBIGINT, SQL_SUCCESS, ""}},
             "boolean");
}

// ---- String storage: varchar ----------------------------------------------------------------

TEST_CASE("convert: varchar to integers") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, strCol("  42 "), SQL_C_SLONG), SQL_SUCCESS);
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 42L);
  CHECK_EQ(cv.run(col, strCol("42.000"), SQL_C_SLONG), SQL_SUCCESS);  // zero fraction accepted silently
  CHECK_EQ(cv.run(col, strCol("42.5"), SQL_C_SLONG), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<long>(cv.as<SQLINTEGER>()), 42L);
  CHECK_EQ(cv.run(col, strCol("-42.5"), SQL_C_SBIGINT), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<long long>(cv.as<SQLBIGINT>()), -42LL);
  CHECK_EQ(cv.run(col, strCol("1e3"), SQL_C_SSHORT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLSMALLINT>()), 1000);
  CHECK_EQ(cv.run(col, strCol("+7"), SQL_C_UTINYINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQLCHAR>()), 7);
  CHECK_EQ(cv.run(col, strCol("18446744073709551615"), SQL_C_UBIGINT), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned long long>(cv.as<SQLUBIGINT>()), 18446744073709551615ULL);
  CHECK_EQ(cv.run(col, strCol("-0.5"), SQL_C_UBIGINT), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<unsigned long long>(cv.as<SQLUBIGINT>()), 0ULL);

  checkTable(cv, col, strCol("abc"), {{SQL_C_SLONG, SQL_ERROR, "22018"}, {SQL_C_DOUBLE, SQL_ERROR, "22018"},
                                      {SQL_C_NUMERIC, SQL_ERROR, "22018"}, {SQL_C_SBIGINT, SQL_ERROR, "22018"}},
             "abc");
  checkTable(cv, col, strCol(""), {{SQL_C_SLONG, SQL_ERROR, "22018"}}, "empty");
  checkTable(cv, col, strCol("12abc"), {{SQL_C_SLONG, SQL_ERROR, "22018"}}, "12abc");
  checkTable(cv, col, strCol("99999999999999999999"),
             {{SQL_C_SBIGINT, SQL_ERROR, "22003"}, {SQL_C_UBIGINT, SQL_ERROR, "22003"}, {SQL_C_DOUBLE, SQL_SUCCESS, ""}},
             "1e20 text");
  checkTable(cv, col, strCol("300"), {{SQL_C_STINYINT, SQL_ERROR, "22003"}, {SQL_C_UTINYINT, SQL_ERROR, "22003"}},
             "300");
  checkTable(cv, col, strCol("-1"), {{SQL_C_ULONG, SQL_ERROR, "22003"}}, "-1 text");
}

TEST_CASE("convert: varchar to SQL_C_BIT") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  checkTable(cv, col, strCol("1"), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "1");
  checkTable(cv, col, strCol("0"), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "0");
  checkTable(cv, col, strCol(" TRUE "), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "TRUE");
  checkTable(cv, col, strCol("false"), {{SQL_C_BIT, SQL_SUCCESS, ""}}, "false");
  checkTable(cv, col, strCol("1.5"), {{SQL_C_BIT, SQL_SUCCESS_WITH_INFO, "01S07"}}, "1.5");
  checkTable(cv, col, strCol("2"), {{SQL_C_BIT, SQL_ERROR, "22003"}}, "2");
  checkTable(cv, col, strCol("-1"), {{SQL_C_BIT, SQL_ERROR, "22003"}}, "-1");
  checkTable(cv, col, strCol("yes"), {{SQL_C_BIT, SQL_ERROR, "22018"}}, "yes");
  CHECK_EQ(cv.run(col, strCol("true"), SQL_C_BIT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<unsigned char>()), 1);
  CHECK_EQ(cv.run(col, strCol("false"), SQL_C_BIT), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<unsigned char>()), 0);
}

TEST_CASE("convert: varchar to floating point and numeric") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, strCol(" -1.25e2 "), SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLDOUBLE>(), -125.0);
  CHECK_EQ(cv.run(col, strCol("NaN"), SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK(cv.as<SQLDOUBLE>() != cv.as<SQLDOUBLE>());
  CHECK_EQ(cv.run(col, strCol("-Infinity"), SQL_C_FLOAT), SQL_SUCCESS);
  CHECK(cv.as<SQLREAL>() < 0 && std::isinf(cv.as<SQLREAL>()));
  CHECK_EQ(cv.run(col, strCol("1e40"), SQL_C_FLOAT), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(col, strCol("1e40"), SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLDOUBLE>(), 1e40);

  CHECK_EQ(cv.run(col, strCol("12.345"), SQL_C_NUMERIC), SQL_SUCCESS_WITH_INFO);  // default scale 0
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(numericValue(cv).toString(), std::string("12"));
  cv.numericScaleSet = true;
  cv.numericScale = 3;
  CHECK_EQ(cv.run(col, strCol("12.345"), SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("12.345"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().sign), 1);
  CHECK_EQ(cv.run(col, strCol("-0.001"), SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("-0.001"));
}

TEST_CASE("convert: varchar to GUID") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, strCol("12345678-9abc-def0-1234-56789abcdef0"), SQL_C_GUID), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLGUID)));
  const SQLGUID g = cv.as<SQLGUID>();
  CHECK_EQ(static_cast<unsigned long>(g.Data1), 0x12345678UL);
  CHECK_EQ(static_cast<unsigned>(g.Data2), 0x9abcu);
  CHECK_EQ(static_cast<unsigned>(g.Data3), 0xdef0u);
  CHECK_EQ(static_cast<int>(g.Data4[0]), 0x12);
  CHECK_EQ(static_cast<int>(g.Data4[1]), 0x34);
  CHECK_EQ(static_cast<int>(g.Data4[7]), 0xf0);
  CHECK_EQ(cv.run(col, strCol("{12345678-9ABC-DEF0-1234-56789ABCDEF0}"), SQL_C_GUID), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQLGUID>().Data1), 0x12345678UL);
  checkTable(cv, col, strCol("not-a-uuid"), {{SQL_C_GUID, SQL_ERROR, "22018"}}, "bad uuid");
  checkTable(cv, col, strCol("12345678-9abc-def0-1234-56789abcdefg"), {{SQL_C_GUID, SQL_ERROR, "22018"}}, "bad hex");
}

TEST_CASE("convert: uuid column") {
  Conv cv;
  TypeMappingOptions tm;
  tm.uuidAsGuid = true;
  const ResultColumn col = makeCol("uuid", tm);
  const Column v = strCol("0e984725-c51c-4bf4-9960-e1c80e27aba0");
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLGUID)));
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQLGUID>().Data1), 0x0e984725UL);
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("0e984725-c51c-4bf4-9960-e1c80e27aba0"));
  checkTable(cv, col, v, {{SQL_C_SLONG, SQL_ERROR, "07006"}, {SQL_C_TYPE_DATE, SQL_ERROR, "07006"},
                          {SQL_C_BINARY, SQL_SUCCESS, ""}},
             "uuid");
  CHECK_EQ(cv.run(makeCol("uuid"), v, SQL_C_GUID), SQL_SUCCESS);  // varchar-exposed uuid still parses
}

TEST_CASE("convert: varchar to datetime structs (char literal rules)") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, strCol("2024-05-06"), SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().year), 2024);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().month), 5);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().day), 6);
  CHECK_EQ(cv.run(col, strCol("2024-05-06 00:00:00"), SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, strCol("2024-05-06 00:00:01"), SQL_C_TYPE_DATE), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(cv.run(col, strCol(" 10:11:12 "), SQL_C_TYPE_TIME), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 10);
  CHECK_EQ(cv.run(col, strCol("10:11:12.5"), SQL_C_TYPE_TIME), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(cv.run(col, strCol("2024-05-06 10:11:12"), SQL_C_TYPE_TIME), SQL_SUCCESS);  // date dropped silently
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().second), 12);
  CHECK_EQ(cv.run(col, strCol("2024-05-06 10:11:12.25"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQL_TIMESTAMP_STRUCT>().fraction), 250000000UL);
  CHECK_EQ(cv.run(col, strCol("2024-05-06"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 0);
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQL_TIMESTAMP_STRUCT>().fraction), 0UL);
  checkTable(cv, col, strCol("yesterday"),
             {{SQL_C_TYPE_DATE, SQL_ERROR, "22018"}, {SQL_C_TYPE_TIME, SQL_ERROR, "22018"},
              {SQL_C_TYPE_TIMESTAMP, SQL_ERROR, "22018"}},
             "yesterday");
}

TEST_CASE("convert: varchar to intervals") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, strCol("5-0"), SQL_C_INTERVAL_YEAR), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.year_month.year), 5u);
  CHECK_EQ(cv.run(col, strCol("1 00:00:30.000"), SQL_C_INTERVAL_DAY_TO_SECOND), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.second), 30u);
  checkTable(cv, col, strCol("1 00:00:30.000"), {{SQL_C_INTERVAL_YEAR, SQL_ERROR, "22018"}}, "ds text to year");
  checkTable(cv, col, strCol("5-0"), {{SQL_C_INTERVAL_DAY, SQL_ERROR, "22018"}}, "ym text to day");
}

// ---- String storage: decimal ----------------------------------------------------------------

TEST_CASE("convert: decimal column") {
  Conv cv;
  const ResultColumn col = makeCol("decimal(10,3)");
  const Column v = strCol("-12345.678");
  CHECK_EQ(cv.run(col, v, SQL_C_NUMERIC), SQL_SUCCESS);  // column precision/scale used
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().precision), 10);
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().scale), 3);
  CHECK_EQ(static_cast<int>(cv.as<SQL_NUMERIC_STRUCT>().sign), 0);
  CHECK_EQ(numericValue(cv).toString(), std::string("-12345.678"));
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("-12345.678"));
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);  // SQL_C_CHAR per mapping rules
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(10));
  CHECK_EQ(cv.run(col, v, SQL_C_SBIGINT), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<long long>(cv.as<SQLBIGINT>()), -12345LL);
  CHECK_EQ(cv.run(col, v, SQL_C_DOUBLE), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQLDOUBLE>(), -12345.678);
  CHECK_EQ(cv.run(col, strCol("1.000"), SQL_C_BIT), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, strCol("12.000"), SQL_C_STINYINT), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY), SQL_SUCCESS);  // raw text bytes
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(10));
  CHECK_EQ(cv.bytes(10), std::string("-12345.678"));
  checkTable(cv, col, v,
             {{SQL_C_TYPE_DATE, SQL_ERROR, "07006"}, {SQL_C_TYPE_TIMESTAMP, SQL_ERROR, "07006"},
              {SQL_C_GUID, SQL_ERROR, "07006"}, {SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_ERROR, "07006"},
              {SQL_C_STINYINT, SQL_ERROR, "22003"}},
             "decimal");
  // Decimal -> single-field interval (exact numerics are allowed).
  CHECK_EQ(cv.run(col, strCol("3.000"), SQL_C_INTERVAL_DAY), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.day), 3u);
  CHECK_EQ(cv.run(col, strCol("-1.500"), SQL_C_INTERVAL_SECOND), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQL_INTERVAL_STRUCT>().interval_sign, SQL_TRUE);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.second), 1u);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.fraction), 500000u);  // precision 6

  // A wide decimal round-trips exactly through SQL_NUMERIC_STRUCT.
  const ResultColumn wide = makeCol("decimal(38,10)");
  CHECK_EQ(cv.run(wide, strCol("1234567890123456789012345678.1234567890"), SQL_C_NUMERIC), SQL_SUCCESS);
  CHECK_EQ(numericValue(cv).toString(), std::string("1234567890123456789012345678.1234567890"));
}

// ---- String storage: date / time / timestamp ------------------------------------------------

TEST_CASE("convert: date column") {
  Conv cv;
  const ResultColumn col = makeCol("date");
  const Column v = strCol("2024-02-29");
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)));
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().year), 2024);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().month), 2);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().day), 29);
  CHECK_EQ(cv.run(col, v, SQL_C_DATE), SQL_SUCCESS);  // legacy type
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().day), 29);
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)));
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  const SQL_TIMESTAMP_STRUCT ts = cv.as<SQL_TIMESTAMP_STRUCT>();
  CHECK_EQ(static_cast<int>(ts.year), 2024);
  CHECK_EQ(static_cast<int>(ts.hour), 0);
  CHECK_EQ(static_cast<int>(ts.minute), 0);
  CHECK_EQ(static_cast<int>(ts.second), 0);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 0UL);
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-02-29"));
  CHECK_EQ(cv.run(col, v, SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("2024-02-29"));
  checkTable(cv, col, v,
             {{SQL_C_TYPE_TIME, SQL_ERROR, "07006"}, {SQL_C_SLONG, SQL_ERROR, "07006"}, {SQL_C_DOUBLE, SQL_ERROR, "07006"},
              {SQL_C_BIT, SQL_ERROR, "07006"}, {SQL_C_NUMERIC, SQL_ERROR, "07006"}, {SQL_C_GUID, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_DAY, SQL_ERROR, "07006"}, {SQL_C_BINARY, SQL_SUCCESS, ""}},
             "date");
  CHECK_EQ(cv.run(col, strCol("-0044-03-15"), SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().year), -44);
  CHECK_EQ(cv.run(col, strCol("100000-01-01"), SQL_C_TYPE_DATE), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22008"));
}

TEST_CASE("convert: time column") {
  Conv cv;
  const ResultColumn col = makeCol("time(3)");
  CHECK_EQ(cv.run(col, strCol("13:14:15.000"), SQL_C_TYPE_TIME), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)));
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 13);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().minute), 14);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().second), 15);
  CHECK_EQ(cv.run(col, strCol("13:14:15.250"), SQL_C_TYPE_TIME), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().second), 15);
  CHECK_EQ(cv.run(col, strCol("13:14:15.250"), SQL_C_TIME), SQL_SUCCESS_WITH_INFO);  // legacy

  // time -> timestamp takes the current date; the time and fraction are preserved.
  CHECK_EQ(cv.run(col, strCol("13:14:15.250"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  const SQL_TIMESTAMP_STRUCT ts = cv.as<SQL_TIMESTAMP_STRUCT>();
  CHECK(ts.year >= 2024);
  CHECK(ts.month >= 1 && ts.month <= 12);
  CHECK(ts.day >= 1 && ts.day <= 31);
  CHECK_EQ(static_cast<int>(ts.hour), 13);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 250000000UL);

  CHECK_EQ(cv.run(col, strCol("13:14:15.250"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("13:14:15.250"));
  checkTable(cv, col, strCol("13:14:15.250"),
             {{SQL_C_TYPE_DATE, SQL_ERROR, "07006"}, {SQL_C_SBIGINT, SQL_ERROR, "07006"}, {SQL_C_GUID, SQL_ERROR, "07006"}},
             "time");
}

TEST_CASE("convert: timestamp column") {
  Conv cv;
  const ResultColumn col = makeCol("timestamp(6)");
  const Column v = strCol("2024-03-10 02:30:00.123456");
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)));
  SQL_TIMESTAMP_STRUCT ts = cv.as<SQL_TIMESTAMP_STRUCT>();
  CHECK_EQ(static_cast<int>(ts.year), 2024);
  CHECK_EQ(static_cast<int>(ts.month), 3);
  CHECK_EQ(static_cast<int>(ts.day), 10);
  CHECK_EQ(static_cast<int>(ts.hour), 2);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<int>(ts.second), 0);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123456000UL);
  CHECK_EQ(cv.run(col, v, SQL_C_TIMESTAMP), SQL_SUCCESS);  // legacy
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)));

  // -> date: time part non-zero -> 01S07; zero -> success.
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_DATE), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().day), 10);
  CHECK_EQ(cv.run(col, strCol("2024-03-10 00:00:00.000000"), SQL_C_TYPE_DATE), SQL_SUCCESS);

  // -> time: date dropped silently, fraction -> 01S07.
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_TIME), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 2);
  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.000000"), SQL_C_TYPE_TIME), SQL_SUCCESS);
  CHECK_EQ(cv.diag.count(), size_t(0));

  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123456"));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.bytes(26), std::string("2024-03-10 02:30:00.123456"));

  // Picosecond precision: text keeps every digit, the struct loses them with 01S07.
  const ResultColumn pico = makeCol("timestamp(12)");
  CHECK_EQ(cv.run(pico, strCol("2024-03-10 02:30:00.123456789012"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<unsigned long>(cv.as<SQL_TIMESTAMP_STRUCT>().fraction), 123456789UL);
  CHECK_EQ(cv.run(pico, strCol("2024-03-10 02:30:00.123456789000"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(cv.run(pico, strCol("2024-03-10 02:30:00.123456789012"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123456789012"));

  CHECK_EQ(cv.run(col, strCol("100000-01-01 00:00:00.000000"), SQL_C_TYPE_TIMESTAMP), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22008"));
  checkTable(cv, col, v,
             {{SQL_C_SLONG, SQL_ERROR, "07006"}, {SQL_C_NUMERIC, SQL_ERROR, "07006"}, {SQL_C_BIT, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_DAY_TO_SECOND, SQL_ERROR, "07006"}},
             "timestamp");
}

TEST_CASE("convert: timestamp with time zone, Utc mode") {
  Conv cv;
  cv.opts.timestampTz = TimestampTzMode::Utc;  // the default is Session
  TypeMappingOptions tm;
  tm.timestampTz = TimestampTzMode::Utc;
  const ResultColumn col = makeCol("timestamp(6) with time zone", tm);

  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.123456 +02:00"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 00:30:00.123456"));
  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.123456 +02:00"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  SQL_TIMESTAMP_STRUCT ts = cv.as<SQL_TIMESTAMP_STRUCT>();
  CHECK_EQ(static_cast<int>(ts.hour), 0);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123456000UL);

  CHECK_EQ(cv.run(col, strCol("2024-01-01 01:00:00.000000 +05:30"), SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("2023-12-31 19:30:00.000000"));
  CHECK_EQ(cv.run(col, strCol("2024-06-01 12:00:00.000000 Etc/UTC"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-06-01 12:00:00.000000"));
  CHECK_EQ(cv.run(col, strCol("2024-06-01 12:00:00.000000 UTC"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 12);

  // -> date after conversion to UTC.
  CHECK_EQ(cv.run(col, strCol("2024-01-01 01:00:00.000000 +05:30"), SQL_C_TYPE_DATE), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().day), 31);
  CHECK_EQ(cv.run(col, strCol("2024-01-01 05:30:00.000000 +05:30"), SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().year), 2024);

  // Picosecond digits survive the UTC conversion in text form.
  const ResultColumn pico = makeCol("timestamp(12) with time zone", tm);
  CHECK_EQ(cv.run(pico, strCol("2024-03-10 02:30:00.123456789012 +02:00"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 00:30:00.123456789012"));
}

TEST_CASE("convert: timestamp with time zone, unknown zone falls back to wall clock with 01000 once") {
  Conv cv;  // default Session mode: an unknown value zone cannot be converted either
  TypeMappingOptions tm;
  const ResultColumn col = makeCol("timestamp(3) with time zone", tm);
  const Column v = strCol("2024-03-10 02:30:00.123 Nowhere/Land");
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01000"));
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123"));
  CHECK_EQ(cv.diag.count(), size_t(1));
  // A second conversion into the same diagnostic area must not add a second record.
  ConvertTarget t;
  t.cType = SQL_C_TYPE_TIMESTAMP;
  t.buffer = cv.buf;
  t.bufferLength = sizeof(cv.buf);
  t.strLenOrInd = &cv.ind;
  CHECK_EQ(convertToC(col, v, 0, t, cv.opts, nullptr, cv.diag, 2, 1), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.diag.count(), size_t(1));
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 2);
}

TEST_CASE("convert: timestamp with time zone, WallClock and String modes") {
  Conv cv;
  cv.opts.timestampTz = TimestampTzMode::WallClock;
  TypeMappingOptions tm;
  tm.timestampTz = TimestampTzMode::WallClock;
  const ResultColumn col = makeCol("timestamp(3) with time zone", tm);
  const Column v = strCol("2024-03-10 02:30:00.123 +02:00");
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123"));
  CHECK_EQ(cv.run(col, v, SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 2);
  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.123 Nowhere/Land"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123"));

  cv.opts.timestampTz = TimestampTzMode::String;
  tm.timestampTz = TimestampTzMode::String;
  const ResultColumn scol = makeCol("timestamp(3) with time zone", tm);
  CHECK_EQ(cv.run(scol, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123 +02:00"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(30));
  CHECK_EQ(cv.run(scol, v, SQL_C_DEFAULT), SQL_SUCCESS);  // exposed as (W)VARCHAR
  CHECK_EQ(cv.wstr(), std::string("2024-03-10 02:30:00.123 +02:00"));
  CHECK_EQ(cv.run(scol, v, SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);  // struct = wall clock
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 2);
}

TEST_CASE("convert: timestamp with named zone (needs zoneinfo)") {
  if (!haveZoneinfo()) return;
  Conv cv;
  cv.opts.timestampTz = TimestampTzMode::Utc;
  const ResultColumn col = makeCol("timestamp(3) with time zone");
  CHECK_EQ(cv.run(col, strCol("2024-01-15 12:00:00.000 America/New_York"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-01-15 17:00:00.000"));
  CHECK_EQ(cv.run(col, strCol("2024-07-15 12:00:00.000 America/New_York"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 16);
}

TEST_CASE("convert: timestamp with time zone, Session mode (default) renders in Europe/Istanbul") {
  Conv cv;
  CHECK(cv.opts.timestampTz == TimestampTzMode::Session);
  CHECK_EQ(cv.opts.sessionTimeZone, std::string("Europe/Istanbul"));
  const ResultColumn col = makeCol("timestamp(3) with time zone");  // default mapping: SQL_TYPE_TIMESTAMP
  CHECK_EQ(static_cast<int>(col.odbc.conciseType), static_cast<int>(SQL_TYPE_TIMESTAMP));

  // Fixed offset: 02:30 +02:00 = 00:30 UTC = 03:30 Istanbul (+03:00). No zone suffix in the text.
  const Column off = strCol("2024-03-10 02:30:00.123 +02:00");
  CHECK_EQ(cv.run(col, off, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 03:30:00.123"));
  CHECK_EQ(cv.run(col, off, SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("2024-03-10 03:30:00.123"));
  CHECK_EQ(cv.run(col, off, SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  SQL_TIMESTAMP_STRUCT ts = cv.as<SQL_TIMESTAMP_STRUCT>();
  CHECK_EQ(static_cast<int>(ts.year), 2024);
  CHECK_EQ(static_cast<int>(ts.day), 10);
  CHECK_EQ(static_cast<int>(ts.hour), 3);
  CHECK_EQ(static_cast<int>(ts.minute), 30);
  CHECK_EQ(static_cast<unsigned long>(ts.fraction), 123000000UL);
  // Day rollover: 22:30 UTC -> 01:30 the next day in Istanbul.
  CHECK_EQ(cv.run(col, strCol("2023-12-31 22:30:00.000 UTC"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-01-01 01:30:00.000"));
  CHECK_EQ(cv.run(col, strCol("2023-12-31 22:30:00.000 UTC"), SQL_C_TYPE_DATE), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<int>(cv.as<SQL_DATE_STRUCT>().year), 2024);
  // A value already in Istanbul time is unchanged.
  CHECK_EQ(cv.run(col, strCol("2024-07-15 12:00:00.000 Europe/Istanbul"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-07-15 12:00:00.000"));
  // Picosecond digits survive in text form.
  const ResultColumn pico = makeCol("timestamp(12) with time zone");
  CHECK_EQ(cv.run(pico, strCol("2024-03-10 02:30:00.123456789012 +02:00"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 03:30:00.123456789012"));

  // Explicit session zone given as a fixed offset.
  cv.opts.sessionTimeZone = "-05:00";
  CHECK_EQ(cv.run(col, off, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-09 19:30:00.123"));
}

TEST_CASE("convert: timestamp with time zone, Session mode with named zones (needs zoneinfo)") {
  if (!haveZoneinfo()) return;
  Conv cv;
  const ResultColumn col = makeCol("timestamp(3) with time zone");
  // 02:30 on 2024-03-10 does not exist in New York (spring-forward gap): resolved with the earlier
  // offset (EST, UTC-5) -> 07:30 UTC -> 10:30 Istanbul.
  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.123 America/New_York"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-03-10 10:30:00.123"));
  // 12:30 EDT (UTC-4) -> 16:30 UTC -> 19:30 Istanbul.
  CHECK_EQ(cv.run(col, strCol("2024-03-10 12:30:00.123 America/New_York"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 19);

  // A DST session zone: Berlin is UTC+2 in summer and UTC+1 in winter.
  cv.opts.sessionTimeZone = "Europe/Berlin";
  CHECK_EQ(cv.run(col, strCol("2024-07-15 12:00:00.000 UTC"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-07-15 14:00:00.000"));
  CHECK_EQ(cv.run(col, strCol("2024-01-15 12:00:00.000 UTC"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-01-15 13:00:00.000"));
  CHECK_EQ(cv.run(col, strCol("2024-01-15 12:00:00.000 Europe/Istanbul"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 10);
}

TEST_CASE("convert: timestamp with time zone, Session mode with an unresolvable zone") {
  Conv cv;
  const ResultColumn col = makeCol("timestamp(3) with time zone");
  // Unknown session zone: value delivered in UTC with one 01000 warning.
  cv.opts.sessionTimeZone = "Nowhere/Land";
  const Column off = strCol("2024-03-10 02:30:00.123 +02:00");
  CHECK_EQ(cv.run(col, off, SQL_C_CHAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01000"));
  CHECK_EQ(cv.str(), std::string("2024-03-10 00:30:00.123"));
  CHECK_EQ(cv.diag.count(), size_t(1));
  CHECK(cv.diag.records()[0].message.find("Session time zone 'Nowhere/Land'") != std::string::npos);
  ConvertTarget t;
  t.cType = SQL_C_TYPE_TIMESTAMP;
  t.buffer = cv.buf;
  t.bufferLength = sizeof(cv.buf);
  t.strLenOrInd = &cv.ind;
  CHECK_EQ(convertToC(col, off, 0, t, cv.opts, nullptr, cv.diag, 2, 1), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.diag.count(), size_t(1));  // once per diagnostic area
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 0);
  // Empty session zone (OS zone undetectable with TimeZone=system): same UTC fallback.
  cv.opts.sessionTimeZone.clear();
  CHECK_EQ(cv.run(col, off, SQL_C_CHAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("2024-03-10 00:30:00.123"));
  // Unknown value zone with a valid session zone: wall clock as sent, value-zone warning.
  cv.opts.sessionTimeZone = "Europe/Istanbul";
  CHECK_EQ(cv.run(col, strCol("2024-03-10 02:30:00.123 Nowhere/Land"), SQL_C_CHAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("2024-03-10 02:30:00.123"));
  CHECK(cv.diag.records()[0].message.find("Time zone 'Nowhere/Land' is unknown") != std::string::npos);
}

TEST_CASE("convert: time with time zone modes") {
  Conv cv;  // TimeTzMode::String by default
  const ResultColumn scol = makeCol("time(6) with time zone");
  const Column v = strCol("12:34:56.123456+05:30");
  CHECK_EQ(cv.run(scol, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("12:34:56.123456+05:30"));
  CHECK_EQ(cv.run(scol, v, SQL_C_TYPE_TIME), SQL_SUCCESS_WITH_INFO);  // wall clock, fraction lost
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 12);

  cv.opts.timeTz = TimeTzMode::Utc;
  TypeMappingOptions tm;
  tm.timeTz = TimeTzMode::Utc;
  const ResultColumn ucol = makeCol("time(6) with time zone", tm);
  CHECK_EQ(cv.run(ucol, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("07:04:56.123456"));
  CHECK_EQ(cv.run(ucol, v, SQL_C_TYPE_TIME), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 7);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().minute), 4);
  CHECK_EQ(cv.run(ucol, strCol("01:00:00.000000-05:00"), SQL_C_TYPE_TIME), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 6);
  CHECK_EQ(cv.run(ucol, strCol("23:00:00.000000-05:00"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("04:00:00.000000"));
  CHECK_EQ(cv.run(ucol, strCol("01:00:00.000000+05:00"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("20:00:00.000000"));
  CHECK_EQ(cv.run(ucol, strCol("01:00:00.000000+05:00"), SQL_C_TYPE_TIMESTAMP), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIMESTAMP_STRUCT>().hour), 20);

  cv.opts.timeTz = TimeTzMode::WallClock;
  tm.timeTz = TimeTzMode::WallClock;
  const ResultColumn wcol = makeCol("time(6) with time zone", tm);
  CHECK_EQ(cv.run(wcol, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("12:34:56.123456"));

  // Session: shifted to the session zone's current offset (Istanbul: +03:00 all year).
  cv.opts.timeTz = TimeTzMode::Session;
  tm.timeTz = TimeTzMode::Session;
  const ResultColumn sesscol = makeCol("time(6) with time zone", tm);
  CHECK_EQ(static_cast<int>(sesscol.odbc.conciseType), static_cast<int>(SQL_TYPE_TIME));
  CHECK_EQ(cv.run(sesscol, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("10:04:56.123456"));
  CHECK_EQ(cv.run(sesscol, strCol("22:00:00.000000+00:00"), SQL_C_TYPE_TIME), SQL_SUCCESS);
  CHECK_EQ(static_cast<int>(cv.as<SQL_TIME_STRUCT>().hour), 1);
  cv.opts.sessionTimeZone = "Nowhere/Land";  // unknown -> UTC + 01000
  CHECK_EQ(cv.run(sesscol, v, SQL_C_CHAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01000"));
  CHECK_EQ(cv.str(), std::string("07:04:56.123456"));
}

// ---- intervals ------------------------------------------------------------------------------

TEST_CASE("convert: interval year to month column") {
  Conv cv;
  TypeMappingOptions tm;
  tm.interval = IntervalMode::Interval;
  const ResultColumn col = makeCol("interval year to month", tm);
  const Column v = strCol("1-2");
  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_YEAR_TO_MONTH), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_INTERVAL_STRUCT)));
  SQL_INTERVAL_STRUCT iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_YEAR_TO_MONTH));
  CHECK_EQ(iv.interval_sign, SQL_FALSE);
  CHECK_EQ(static_cast<unsigned>(iv.intval.year_month.year), 1u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.year_month.month), 2u);
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQL_INTERVAL_STRUCT)));

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_MONTH), SQL_SUCCESS);
  iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_MONTH));
  CHECK_EQ(static_cast<unsigned>(iv.intval.year_month.month), 14u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.year_month.year), 0u);

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_YEAR), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.year_month.year), 1u);
  CHECK_EQ(cv.run(col, strCol("-3-0"), SQL_C_INTERVAL_YEAR), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQL_INTERVAL_STRUCT>().interval_sign, SQL_TRUE);

  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("1-2"));
  checkTable(cv, col, v,
             {{SQL_C_INTERVAL_DAY_TO_SECOND, SQL_ERROR, "07006"}, {SQL_C_INTERVAL_SECOND, SQL_ERROR, "07006"},
              {SQL_C_SLONG, SQL_ERROR, "07006"}, {SQL_C_TYPE_DATE, SQL_ERROR, "07006"}, {SQL_C_BINARY, SQL_SUCCESS, ""}},
             "interval ym");
  // Exposed as text (IntervalMode::String) the same conversions still work.
  const ResultColumn tcol = makeCol("interval year to month");
  CHECK_EQ(cv.run(tcol, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("1-2"));
  CHECK_EQ(cv.run(tcol, v, SQL_C_INTERVAL_MONTH), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.year_month.month), 14u);
}

TEST_CASE("convert: interval day to second column") {
  Conv cv;
  TypeMappingOptions tm;
  tm.interval = IntervalMode::Interval;
  const ResultColumn col = makeCol("interval day to second", tm);
  const Column v = strCol("3 04:05:06.789");
  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_DAY_TO_SECOND), SQL_SUCCESS);
  SQL_INTERVAL_STRUCT iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_DAY_TO_SECOND));
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.day), 3u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.hour), 4u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.minute), 5u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.second), 6u);
  // Without an ARD precision the ODBC default seconds precision 6 applies, not the column's 3
  // (review finding 17): .789 -> 789000 microseconds.
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.fraction), 789000u);

  cv.numericPrecision = 9;  // ARD SQL_DESC_PRECISION wins
  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_DAY_TO_SECOND), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.fraction), 789000000u);
  cv.numericPrecision = 1;
  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_DAY_TO_SECOND), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.fraction), 7u);
  cv.numericPrecision = 0;

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_SECOND), SQL_SUCCESS);
  iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_SECOND));
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.second), 3u * 86400 + 4 * 3600 + 5 * 60 + 6);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.fraction), 789000u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.day), 0u);

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_HOUR_TO_MINUTE), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01S07"));
  iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.hour), 76u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.minute), 5u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.second), 0u);

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_MINUTE_TO_SECOND), SQL_SUCCESS);
  iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.minute), 76u * 60 + 5);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.second), 6u);

  CHECK_EQ(cv.run(col, v, SQL_C_INTERVAL_DAY), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.day), 3u);
  CHECK_EQ(cv.run(col, strCol("3 00:00:00.000"), SQL_C_INTERVAL_DAY), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, strCol("3 04:00:00.000"), SQL_C_INTERVAL_DAY_TO_HOUR), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, strCol("3 04:05:00.000"), SQL_C_INTERVAL_DAY_TO_MINUTE), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, strCol("3 04:05:06.000"), SQL_C_INTERVAL_HOUR_TO_SECOND), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.hour), 76u);

  CHECK_EQ(cv.run(col, strCol("-1 00:00:00.000"), SQL_C_INTERVAL_DAY_TO_SECOND), SQL_SUCCESS);
  CHECK_EQ(cv.as<SQL_INTERVAL_STRUCT>().interval_sign, SQL_TRUE);
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("3 04:05:06.789"));
  checkTable(cv, col, v,
             {{SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_ERROR, "07006"}, {SQL_C_INTERVAL_MONTH, SQL_ERROR, "07006"},
              {SQL_C_DOUBLE, SQL_ERROR, "07006"}, {SQL_C_TYPE_TIME, SQL_ERROR, "07006"}},
             "interval ds");
  // Leading field overflow.
  CHECK_EQ(cv.run(col, strCol("4294967295 01:00:00.000"), SQL_C_INTERVAL_HOUR), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22015"));
}

TEST_CASE("convert: integer to single-field intervals") {
  Conv cv;
  const ResultColumn col = makeCol("integer");
  CHECK_EQ(cv.run(col, intCol(5), SQL_C_INTERVAL_DAY), SQL_SUCCESS);
  SQL_INTERVAL_STRUCT iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<int>(iv.interval_type), static_cast<int>(SQL_IS_DAY));
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.day), 5u);
  CHECK_EQ(cv.run(col, intCol(-7), SQL_C_INTERVAL_MONTH), SQL_SUCCESS);
  iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(iv.interval_sign, SQL_TRUE);
  CHECK_EQ(static_cast<unsigned>(iv.intval.year_month.month), 7u);
  CHECK_EQ(cv.run(col, intCol(90), SQL_C_INTERVAL_SECOND), SQL_SUCCESS);
  CHECK_EQ(static_cast<unsigned>(cv.as<SQL_INTERVAL_STRUCT>().intval.day_second.second), 90u);
  checkTable(cv, col, intCol(5),
             {{SQL_C_INTERVAL_DAY_TO_HOUR, SQL_ERROR, "07006"}, {SQL_C_INTERVAL_YEAR_TO_MONTH, SQL_ERROR, "07006"}},
             "int to multi-field interval");
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(4294967296LL), SQL_C_INTERVAL_YEAR), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22015"));
}

// ---- binary storage -------------------------------------------------------------------------

TEST_CASE("convert: varbinary column") {
  Conv cv;
  const ResultColumn col = makeCol("varbinary");
  const Column v = binCol(std::string("\x01\x02\xff\x00\x7f", 5));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(5));
  CHECK_EQ(cv.bytes(5), std::string("\x01\x02\xff\x00\x7f", 5));
  CHECK_EQ(cv.run(col, v, SQL_C_DEFAULT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(5));
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("0102FF007F"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(10));
  CHECK_EQ(cv.run(col, v, SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("0102FF007F"));
  checkTable(cv, col, v,
             {{SQL_C_SLONG, SQL_ERROR, "07006"}, {SQL_C_DOUBLE, SQL_ERROR, "07006"}, {SQL_C_BIT, SQL_ERROR, "07006"},
              {SQL_C_NUMERIC, SQL_ERROR, "07006"}, {SQL_C_TYPE_DATE, SQL_ERROR, "07006"}, {SQL_C_GUID, SQL_ERROR, "07006"},
              {SQL_C_INTERVAL_DAY, SQL_ERROR, "07006"}},
             "varbinary");
  // Empty binary value.
  CHECK_EQ(cv.run(col, binCol(""), SQL_C_BINARY), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(0));
  CHECK_EQ(cv.run(col, binCol(""), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string());
}

// ---- chunking -------------------------------------------------------------------------------

TEST_CASE("convert: SQL_C_CHAR chunking never splits UTF-8 sequences") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn col = makeCol("varchar");
  const std::string text = "Türkçe ğüşiöç 日本語 😀!";
  const Column v = strCol(text);
  std::string assembled;
  size_t expectedRemaining = text.size();
  int calls = 0;
  for (;;) {
    const SQLRETURN rc = cv.run(col, v, SQL_C_CHAR, 5);
    if (rc == SQL_NO_DATA) break;
    REQUIRE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
    CHECK_EQ(static_cast<size_t>(cv.ind), expectedRemaining);  // remaining bytes before this call
    const std::string chunk = cv.str();
    CHECK(chunk.size() <= 4);
    CHECK(utf8IsValid(chunk));
    if (rc == SQL_SUCCESS_WITH_INFO) {
      CHECK_EQ(cv.state(), std::string("01004"));
      CHECK(!chunk.empty());
    }
    assembled += chunk;
    expectedRemaining -= chunk.size();
    if (++calls > 100) break;
  }
  CHECK_EQ(assembled, text);
  CHECK_EQ(cv.offset, SIZE_MAX);
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 5), SQL_NO_DATA);  // stays exhausted
}

TEST_CASE("convert: SQL_C_WCHAR chunking never splits surrogate pairs") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn col = makeCol("varchar");
  const std::string text = "a😀b😀😀c日本";
  const Column v = strCol(text);
  std::string assembled;
  int calls = 0;
  // 3 units of buffer = 2 data units + terminator: an emoji needs both units in UTF-16.
  const SQLLEN bufLen = static_cast<SQLLEN>(3 * sizeof(SQLWCHAR));
  size_t expectedRemainingBytes = wideUnitsFor(text) * sizeof(SQLWCHAR);
  for (;;) {
    const SQLRETURN rc = cv.run(col, v, SQL_C_WCHAR, bufLen);
    if (rc == SQL_NO_DATA) break;
    REQUIRE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
    CHECK_EQ(static_cast<size_t>(cv.ind), expectedRemainingBytes);
    const std::string chunk = cv.wstr();
    CHECK(!chunk.empty());
    CHECK(chunk.find("\xEF\xBF\xBD") == std::string::npos);  // no U+FFFD from split surrogates
    assembled += chunk;
    expectedRemainingBytes -= wideUnitsFor(chunk) * sizeof(SQLWCHAR);
    if (++calls > 100) break;
  }
  CHECK_EQ(assembled, text);
  CHECK_EQ(cv.run(col, v, SQL_C_WCHAR, bufLen), SQL_NO_DATA);
}

TEST_CASE("convert: SQL_C_BINARY chunking") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn col = makeCol("varbinary");
  const std::string bytes("\x00\x01\x02\x03\x04\x05\x06", 7);
  const Column v = binCol(bytes);
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 3), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01004"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(7));
  CHECK_EQ(cv.bytes(3), bytes.substr(0, 3));
  CHECK_EQ(cv.offset, size_t(3));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 3), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(4));
  CHECK_EQ(cv.bytes(3), bytes.substr(3, 3));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 3), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(1));
  CHECK_EQ(cv.bytes(1), bytes.substr(6, 1));
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 3), SQL_NO_DATA);

  // A String column delivers its raw UTF-8 bytes to SQL_C_BINARY.
  cv.offset = 0;
  const Column s = strCol("héllo");
  CHECK_EQ(cv.run(makeCol("varchar"), s, SQL_C_BINARY, 4), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(6));
  CHECK_EQ(cv.bytes(4), std::string("h\xC3\xA9l"));
  CHECK_EQ(cv.run(makeCol("varchar"), s, SQL_C_BINARY, 4), SQL_SUCCESS);
  CHECK_EQ(cv.bytes(2), std::string("lo"));
}

TEST_CASE("convert: chunking edge cases (zero-length buffer, null buffer, bound columns)") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn col = makeCol("varchar");
  const Column v = strCol("abcdef");
  // Buffer with room for the terminator only: nothing consumed, 01004, full length reported.
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 1), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01004"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(6));
  CHECK_EQ(cv.str(), std::string());
  CHECK_EQ(cv.offset, size_t(0));
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 0), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.offset, size_t(0));
  CHECK_EQ(cv.run(col, v, SQL_C_WCHAR, static_cast<SQLLEN>(sizeof(SQLWCHAR))), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)));
  CHECK_EQ(cv.offset, size_t(0));
  // Null buffer: length only, nothing consumed, no warning.
  cv.nullBuffer = true;
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 100), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(6));
  CHECK_EQ(cv.offset, size_t(0));
  cv.nullBuffer = false;
  // Exact fit (6 chars + NUL) completes in one call.
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 7), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("abcdef"));
  CHECK_EQ(cv.offset, SIZE_MAX);
  // Empty string: one successful call, then SQL_NO_DATA.
  cv.offset = 0;
  CHECK_EQ(cv.run(col, strCol(""), SQL_C_CHAR, 4), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(0));
  CHECK_EQ(cv.str(), std::string());
  CHECK_EQ(cv.run(col, strCol(""), SQL_C_CHAR, 4), SQL_NO_DATA);

  // Bound column (no chunk state): every call truncates from the start.
  cv.chunked = false;
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 4), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("abc"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(6));
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 4), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("abc"));
  // Negative buffer lengths are treated as zero rather than trusted.
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, -5), SQL_SUCCESS_WITH_INFO);
}

TEST_CASE("convert: fixed-size targets and SQLGetData re-reads") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn col = makeCol("integer");
  CHECK_EQ(cv.run(col, intCol(9), SQL_C_SLONG), SQL_SUCCESS);
  CHECK_EQ(cv.run(col, intCol(9), SQL_C_SLONG), SQL_NO_DATA);  // already delivered
  // A failed conversion does not consume the column: the app may retry with another type.
  cv.offset = 0;
  CHECK_EQ(cv.run(col, intCol(70000), SQL_C_SSHORT), SQL_ERROR);
  CHECK_EQ(cv.offset, size_t(0));
  CHECK_EQ(cv.run(col, intCol(70000), SQL_C_SLONG), SQL_SUCCESS);
  CHECK_EQ(cv.offset, SIZE_MAX);
  // Warnings still deliver the value and consume the column.
  cv.offset = 0;
  CHECK_EQ(cv.run(makeCol("double"), dblCol(2.5), SQL_C_SLONG), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.offset, SIZE_MAX);
}

TEST_CASE("convert: NULL handling") {
  Conv cv;
  const ResultColumn col = makeCol("varchar");
  CHECK_EQ(cv.run(col, nullCol(StorageKind::String), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  CHECK_EQ(cv.run(makeCol("bigint"), nullCol(StorageKind::Int64), SQL_C_SBIGINT), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  CHECK_EQ(cv.run(makeCol("date"), nullCol(StorageKind::String), SQL_C_TYPE_DATE), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  // Missing indicator -> 22002.
  cv.nullIndicator = true;
  CHECK_EQ(cv.run(col, nullCol(StorageKind::String), SQL_C_CHAR), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22002"));
  cv.nullIndicator = false;
  // With SQLGetData chunk state: SQL_NULL_DATA once, then SQL_NO_DATA.
  cv.chunked = true;
  CHECK_EQ(cv.run(col, nullCol(StorageKind::String), SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  CHECK_EQ(cv.run(col, nullCol(StorageKind::String), SQL_C_WCHAR), SQL_NO_DATA);
}

TEST_CASE("convert: invalid C type is HY003") {
  Conv cv;
  CHECK_EQ(cv.run(makeCol("varchar"), strCol("x"), static_cast<SQLSMALLINT>(9999)), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("HY003"));
}

TEST_CASE("convert: json / complex / ipaddress columns render raw text") {
  Conv cv;
  CHECK_EQ(cv.run(makeCol("json"), strCol("{\"a\":[1,2]}"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("{\"a\":[1,2]}"));
  CHECK_EQ(cv.run(makeCol("array(integer)"), strCol("[1,2,3]"), SQL_C_WCHAR), SQL_SUCCESS);
  CHECK_EQ(cv.wstr(), std::string("[1,2,3]"));
  CHECK_EQ(cv.run(makeCol("ipaddress"), strCol("10.0.0.1"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("10.0.0.1"));
  CHECK_EQ(cv.run(makeCol("ipaddress"), strCol("10.0.0.1"), SQL_C_SLONG), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22018"));
  CHECK_EQ(cv.run(makeCol("HyperLogLog"), strCol("opaque"), SQL_C_CHAR), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("opaque"));
}

// ---- cellToText -----------------------------------------------------------------------------

TEST_CASE("convert: cellToText") {
  ConvertOptions opts;
  opts.timestampTz = TimestampTzMode::Utc;
  CHECK_EQ(cellToText(makeCol("boolean"), boolCol(true), 0, opts), std::string("1"));
  CHECK_EQ(cellToText(makeCol("boolean"), boolCol(false), 0, opts), std::string("0"));
  CHECK_EQ(cellToText(makeCol("bigint"), intCol(-42), 0, opts), std::string("-42"));
  CHECK_EQ(cellToText(makeCol("double"), dblCol(1.5), 0, opts), std::string("1.5"));
  CHECK_EQ(cellToText(makeCol("double"), dblCol(1e300), 0, opts), std::string("1e+300"));
  CHECK_EQ(cellToText(makeCol("real"), dblCol(static_cast<double>(3.3f)), 0, opts), std::string("3.3"));
  CHECK_EQ(cellToText(makeCol("varbinary"), binCol("\xAB\xCD"), 0, opts), std::string("ABCD"));
  CHECK_EQ(cellToText(makeCol("decimal(5,2)"), strCol("123.45"), 0, opts), std::string("123.45"));
  CHECK_EQ(cellToText(makeCol("varchar"), strCol("héllo"), 0, opts), std::string("héllo"));
  CHECK_EQ(cellToText(makeCol("varchar"), nullCol(StorageKind::String), 0, opts), std::string());
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 +02:00"), 0, opts),
           std::string("2024-03-10 00:30:00.123"));
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 Nowhere/Land"), 0, opts),
           std::string("2024-03-10 02:30:00.123"));
  opts.timestampTz = TimestampTzMode::WallClock;
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 +02:00"), 0, opts),
           std::string("2024-03-10 02:30:00.123"));
  opts.timestampTz = TimestampTzMode::String;
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 +02:00"), 0, opts),
           std::string("2024-03-10 02:30:00.123 +02:00"));
  CHECK_EQ(cellToText(makeCol("time(3) with time zone"), strCol("12:34:56.123+05:30"), 0, opts),
           std::string("12:34:56.123+05:30"));
  opts.timeTz = TimeTzMode::Utc;
  CHECK_EQ(cellToText(makeCol("time(3) with time zone"), strCol("12:34:56.123+05:30"), 0, opts),
           std::string("07:04:56.123"));
  CHECK_EQ(cellToText(makeCol("interval day to second"), strCol("3 04:05:06.789"), 0, opts),
           std::string("3 04:05:06.789"));
}

TEST_CASE("convert: cellToText agrees with the Session mode (default)") {
  const ConvertOptions defaults;
  CHECK(defaults.timestampTz == TimestampTzMode::Session);
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 +02:00"), 0, defaults),
           std::string("2024-03-10 03:30:00.123"));
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 Nowhere/Land"), 0, defaults),
           std::string("2024-03-10 02:30:00.123"));
  CHECK_EQ(cellToText(makeCol("time(3) with time zone"), strCol("12:34:56.123+05:30"), 0, defaults),
           std::string("12:34:56.123+05:30"));  // TimeWithTimeZoneMode default stays string
  ConvertOptions opts;
  opts.sessionTimeZone = "Nowhere/Land";
  CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 +02:00"), 0, opts),
           std::string("2024-03-10 00:30:00.123"));
  opts.timeTz = TimeTzMode::Session;
  opts.sessionTimeZone = "Europe/Istanbul";
  CHECK_EQ(cellToText(makeCol("time(3) with time zone"), strCol("12:34:56.123+05:30"), 0, opts),
           std::string("10:04:56.123"));
  if (haveZoneinfo()) {
    CHECK_EQ(cellToText(makeCol("timestamp(3) with time zone"), strCol("2024-03-10 02:30:00.123 America/New_York"), 0,
                        defaults),
             std::string("2024-03-10 10:30:00.123"));
  }
}

// ---- multi-row columns ----------------------------------------------------------------------

TEST_CASE("convert: row index selects the right cell") {
  Column c(StorageKind::String);
  c.appendBytes("first");
  c.appendNull();
  c.appendBytes("third");
  const ResultColumn col = makeCol("varchar");
  ConvertOptions opts;
  Diag diag;
  char buf[32];
  SQLLEN ind = 0;
  ConvertTarget t;
  t.cType = SQL_C_CHAR;
  t.buffer = buf;
  t.bufferLength = sizeof(buf);
  t.strLenOrInd = &ind;
  CHECK_EQ(convertToC(col, c, 2, t, opts, nullptr, diag, 3, 1), SQL_SUCCESS);
  CHECK_EQ(std::string(buf), std::string("third"));
  CHECK_EQ(convertToC(col, c, 1, t, opts, nullptr, diag, 2, 1), SQL_SUCCESS);
  CHECK_EQ(ind, static_cast<SQLLEN>(SQL_NULL_DATA));
  CHECK_EQ(convertToC(col, c, 0, t, opts, nullptr, diag, 1, 1), SQL_SUCCESS);
  CHECK_EQ(std::string(buf), std::string("first"));
  // Diagnostics carry the supplied row/column numbers.
  Column n(StorageKind::String);
  n.appendBytes("nope");
  CHECK_EQ(convertToC(col, n, 0, ConvertTarget{SQL_C_SLONG, buf, sizeof(buf), &ind, 0, 0, false}, opts, nullptr, diag, 7,
                      3),
           SQL_ERROR);
  REQUIRE(diag.count() >= 1);
  CHECK_EQ(diag.records().back().rowNumber, static_cast<SQLLEN>(7));
  CHECK_EQ(diag.records().back().columnNumber, static_cast<SQLINTEGER>(3));
}

// ---- review regressions (convert_types_sql) ----------------------------------------------------

TEST_CASE("convert: numeric to SQL_C_CHAR never loses whole digits (review 16)") {
  Conv cv;
  // bigint 123456 into a 4-byte buffer: 22003, nothing written, no partial number.
  const ResultColumn big = makeCol("bigint");
  CHECK_EQ(cv.run(big, intCol(123456), SQL_C_CHAR, 4), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.buf[0], static_cast<unsigned char>(0xEE));
  // Exactly fitting (7 bytes incl. NUL) succeeds.
  CHECK_EQ(cv.run(big, intCol(-12345), SQL_C_CHAR, 7), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("-12345"));
  CHECK_EQ(cv.run(big, intCol(-12345), SQL_C_CHAR, 6), SQL_ERROR);  // the sign counts
  CHECK_EQ(cv.state(), std::string("22003"));

  // double: fractional digits may be dropped with 01004, whole digits may not.
  const ResultColumn dbl = makeCol("double");
  CHECK_EQ(cv.run(dbl, dblCol(1234567.5), SQL_C_CHAR, 8), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01004"));
  CHECK_EQ(cv.str(), std::string("1234567"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(9));  // untruncated length
  CHECK_EQ(cv.run(dbl, dblCol(1234567.5), SQL_C_CHAR, 7), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(dbl, dblCol(3.14159), SQL_C_CHAR, 5), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("3.14"));

  // decimal text, same rules, also for SQL_C_WCHAR (sizes in SQLWCHAR units).
  const ResultColumn dec = makeCol("decimal(10,3)");
  CHECK_EQ(cv.run(dec, strCol("-123.456"), SQL_C_WCHAR, static_cast<SQLLEN>(7 * sizeof(SQLWCHAR))),
           SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.wstr(), std::string("-123.4"));
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(8 * sizeof(SQLWCHAR)));
  CHECK_EQ(cv.run(dec, strCol("-123.456"), SQL_C_WCHAR, static_cast<SQLLEN>(4 * sizeof(SQLWCHAR))), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));

  // bit: one digit needs a 2-byte buffer.
  CHECK_EQ(cv.run(makeCol("boolean"), boolCol(true), SQL_C_CHAR, 1), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
}

TEST_CASE("convert: datetime to SQL_C_CHAR needs the whole date/time (review 16)") {
  Conv cv;
  const ResultColumn date = makeCol("date");
  CHECK_EQ(cv.run(date, strCol("2024-01-05"), SQL_C_CHAR, 5), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(date, strCol("2024-01-05"), SQL_C_CHAR, 11), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("2024-01-05"));

  const ResultColumn ts = makeCol("timestamp(3)");
  CHECK_EQ(cv.run(ts, strCol("2024-01-05 10:20:30.123"), SQL_C_CHAR, 19), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(ts, strCol("2024-01-05 10:20:30.123"), SQL_C_CHAR, 20), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.state(), std::string("01004"));
  CHECK_EQ(cv.str(), std::string("2024-01-05 10:20:30"));
  CHECK_EQ(cv.run(ts, strCol("2024-01-05 10:20:30.123"), SQL_C_CHAR, 22), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("2024-01-05 10:20:30.1"));

  const ResultColumn tm = makeCol("time(3)");
  CHECK_EQ(cv.run(tm, strCol("10:20:30.500"), SQL_C_CHAR, 8), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(tm, strCol("10:20:30.500"), SQL_C_CHAR, 9), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("10:20:30"));

  // Character SQL types keep the byte-stream rules: cut anywhere with 01004.
  CHECK_EQ(cv.run(makeCol("varchar"), strCol("123456"), SQL_C_CHAR, 4), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("123"));
}

TEST_CASE("convert: numeric SQLGetData is never chunked (review 16)") {
  Conv cv;
  cv.chunked = true;
  const ResultColumn dbl = makeCol("double");
  cv.offset = 0;
  CHECK_EQ(cv.run(dbl, dblCol(1234567.5), SQL_C_CHAR, 8), SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(cv.str(), std::string("1234567"));
  // The value was delivered as a whole: the next call reports SQL_NO_DATA, not ".5".
  CHECK_EQ(cv.run(dbl, dblCol(1234567.5), SQL_C_CHAR, 8), SQL_NO_DATA);
  // A failed (22003) call consumes nothing, so a retry with a larger buffer works.
  cv.offset = 0;
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(123456), SQL_C_CHAR, 4), SQL_ERROR);
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(123456), SQL_C_CHAR, 16), SQL_SUCCESS);
  CHECK_EQ(cv.str(), std::string("123456"));
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(123456), SQL_C_CHAR, 16), SQL_NO_DATA);
  // Numeric SQL_C_BINARY: whole value or 22003, never a cut native integer.
  cv.offset = 0;
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(1), SQL_C_BINARY, 4), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(1), SQL_C_BINARY, 8), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(8));
  CHECK_EQ(cv.run(makeCol("bigint"), intCol(1), SQL_C_BINARY, 8), SQL_NO_DATA);
}

TEST_CASE("convert: SQL_GUID column to SQL_C_BINARY yields the 16-byte GUID (review 45)") {
  Conv cv;
  TypeMappingOptions tm;
  tm.uuidAsGuid = true;
  const ResultColumn col = makeCol("uuid", tm);
  const Column v = strCol("12345678-1234-1234-1234-123456789abc");
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 16), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(sizeof(SQLGUID)));
  SQLGUID viaBinary;
  std::memcpy(&viaBinary, cv.buf, sizeof(viaBinary));
  CHECK_EQ(cv.run(col, v, SQL_C_GUID), SQL_SUCCESS);
  CHECK(std::memcmp(&viaBinary, cv.buf, sizeof(SQLGUID)) == 0);
  CHECK_EQ(static_cast<unsigned long>(viaBinary.Data1), 0x12345678ul);
  CHECK_EQ(cv.run(col, v, SQL_C_BINARY, 15), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(col, strCol("not-a-uuid"), SQL_C_BINARY, 16), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22018"));
  // SQL_GUID to SQL_C_CHAR needs all 36 characters.
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 36), SQL_ERROR);
  CHECK_EQ(cv.state(), std::string("22003"));
  CHECK_EQ(cv.run(col, v, SQL_C_CHAR, 37), SQL_SUCCESS);
  // uuid exposed as text (default) keeps the raw text bytes.
  CHECK_EQ(cv.run(makeCol("uuid"), v, SQL_C_BINARY, 64), SQL_SUCCESS);
  CHECK_EQ(cv.ind, static_cast<SQLLEN>(36));
}

TEST_CASE("convert: interval seconds default to precision 6 (review 17)") {
  Conv cv;
  TypeMappingOptions tm;
  tm.interval = IntervalMode::Interval;
  const ResultColumn col = makeCol("interval day to second", tm);
  CHECK_EQ(cv.run(col, strCol("0 00:00:01.500"), SQL_C_INTERVAL_SECOND), SQL_SUCCESS);
  const SQL_INTERVAL_STRUCT iv = cv.as<SQL_INTERVAL_STRUCT>();
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.second), 1u);
  CHECK_EQ(static_cast<unsigned>(iv.intval.day_second.fraction), 500000u);
}
