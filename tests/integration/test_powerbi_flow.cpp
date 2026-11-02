// Power BI Desktop replay: the exact ODBC call sequence the Power Query ODBC connector (Mashup
// engine) issues when a user connects, browses the navigator, previews a table and imports it.
//
//   1. SQLAllocHandle(ENV) + SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION = SQL_OV_ODBC3_80)
//   2. SQLAllocHandle(DBC) + SQLSetConnectAttrW(SQL_ATTR_LOGIN_TIMEOUT) + SQLDriverConnectW(NOPROMPT)
//   3. ~35 SQLGetInfoW probes (driver/DBMS identity, quoting, catalogs, SQL-92 and CONVERT
//      capabilities, getdata extensions, transactions, identifier limits)
//   4. SQLGetFunctions(SQL_API_ODBC3_ALL_FUNCTIONS)
//   5. SQLGetTypeInfoW(SQL_ALL_TYPES) read completely with SQLGetData
//   6. Navigator: SQLTablesW(SQL_ALL_CATALOGS), SQLTablesW(catalog), SQLTablesW(catalog, schema,
//      "%", "TABLE,VIEW"), SQLColumnsW, SQLPrimaryKeysW, SQLForeignKeysW, SQLStatisticsW
//   7. Preview: SQLPrepareW("select ... limit 1000"), SQLNumResultCols + SQLDescribeColW +
//      SQLColAttributeW per column BEFORE SQLExecute, then SQLExecute, SQLBindCol (SQL_C_SBIGINT,
//      SQL_C_WCHAR, SQL_C_DOUBLE, SQL_C_TYPE_TIMESTAMP), SQLSetStmtAttrW(ROW_ARRAY_SIZE,
//      ROWS_FETCHED_PTR, ROW_STATUS_PTR) and a SQLFetchScroll(SQL_FETCH_NEXT) loop
//   8. Import: the same query without the limit on the same statement (bindings kept across
//      SQLFreeStmt(SQL_CLOSE)), timed; a rows/sec summary line is printed
//   9. Long text: SQLGetData of long text columns in small SQL_C_WCHAR chunks
//  10. SQLFreeStmt(SQL_CLOSE / SQL_UNBIND), SQLFreeHandle(STMT), SQLDisconnect
//
// Against the mock the values are asserted exactly (big_table / types_demo of mock_trino.py);
// against a real Trino (SQL_DBMS_VER without "mock") tpch.tiny.orders stands in for the preview,
// tpch.sf1.orders (1.5M rows) for the import, and domain checks replace exact values.
#include "integration/odbc_loader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// Rowset size of the block cursor. Deliberately not a divisor of the mock's 1000-row pages or of
/// the 1000-row preview, so rowsets straddle page boundaries and the last rowset is short.
constexpr SQLULEN kRowset = 384;
/// SQLWCHAR units per bound text cell (Power BI binds short text columns, and reads long ones with
/// SQLGetData). Every bound text value of the scenarios below is shorter than this.
constexpr size_t kTextUnits = 128;
constexpr SQLLEN kUnit = static_cast<SQLLEN>(sizeof(SQLWCHAR));
constexpr uintptr_t kLoginTimeoutSeconds = 15;
constexpr int kPreviewLimit = 1000;
/// Buffer of the chunked SQLGetData calls: 4 data units + terminator, so every realistic text
/// needs several calls (Power BI loops the same way with a larger buffer).
constexpr size_t kChunkUnits = 5;
const char* const kUnicodeText = "Türkçe ğüşiöç 日本語 😀";

// ------------------------------------------------------------------------------------------------
// Scenario description (mock vs real Trino)
// ------------------------------------------------------------------------------------------------

struct ColumnSpec {
  const char* name;
  SQLSMALLINT sqlType;  // SQLDescribeCol DATA_TYPE with the default connection options
  SQLULEN size;
  SQLSMALLINT digits;
  const char* typeName;  // SQL_DESC_TYPE_NAME / SQLColumns TYPE_NAME
};

struct TableSpec {
  const char* name;
  const char* type;
};

struct Target {
  bool mock = false;
  std::string catalog;
  std::string schema;
  std::string table;                     // navigator selection = preview source
  std::vector<TableSpec> schemaTables;   // SQLTables(catalog, schema, "%", "TABLE,VIEW"), in order
  std::vector<std::string> tableColumns; // SQLColumns(catalog, schema, table), ordinal order
  std::vector<ColumnSpec> preview;       // selected columns: SBIGINT, WCHAR, DOUBLE, TYPE_TIMESTAMP, WCHAR
  std::string bigCatalog, bigSchema, bigTable;
  int64_t bigRows = 0;
  std::string longTable;                 // long-text phase (catalog/schema as the preview)
  std::vector<const char*> longColumns;  // SBIGINT, DOUBLE, TYPE_TIMESTAMP bound; two texts via SQLGetData
  int64_t longRows = 0;
};

Target mockTarget() {
  Target t;
  t.mock = true;
  t.catalog = "mock";
  t.schema = "demo";
  t.table = "big_table";
  t.schemaTables = {{"big_table", "TABLE"}, {"types_demo", "TABLE"}, {"demo_view", "VIEW"}};
  t.tableColumns = {"id", "name", "value", "ts", "amount"};
  t.preview = {
      {"id", SQL_BIGINT, 19, 0, "bigint"},
      {"name", SQL_WVARCHAR, 65535, 0, "varchar"},
      {"value", SQL_DOUBLE, 15, 0, "double"},
      {"ts", SQL_TYPE_TIMESTAMP, 23, 3, "timestamp(3)"},
      {"amount", SQL_DECIMAL, 18, 4, "decimal(18,4)"},
  };
  t.bigCatalog = "mock";
  t.bigSchema = "demo";
  t.bigTable = "big_table";
  t.bigRows = 100000;  // BIG_TABLE_DEFAULT_ROWS of the mock
  t.longTable = "types_demo";
  t.longColumns = {"c_bigint", "c_double", "c_timestamp3", "c_varchar", "c_json"};
  t.longRows = 3;
  return t;
}

Target trinoTarget() {
  Target t;
  t.mock = false;
  t.catalog = "tpch";
  t.schema = "tiny";
  t.table = "orders";
  t.schemaTables = {{"customer", "TABLE"}, {"lineitem", "TABLE"}, {"nation", "TABLE"}, {"orders", "TABLE"},
                    {"part", "TABLE"},     {"partsupp", "TABLE"}, {"region", "TABLE"}, {"supplier", "TABLE"}};
  t.tableColumns = {"orderkey", "custkey", "orderstatus", "totalprice", "orderdate",
                    "orderpriority", "clerk", "shippriority", "comment"};
  t.preview = {
      {"orderkey", SQL_BIGINT, 19, 0, "bigint"},
      {"comment", SQL_WVARCHAR, 79, 0, "varchar(79)"},
      {"totalprice", SQL_DOUBLE, 15, 0, "double"},
      {"orderdate", SQL_TYPE_DATE, 10, 0, "date"},
      {"orderpriority", SQL_WVARCHAR, 15, 0, "varchar(15)"},
  };
  t.bigCatalog = "tpch";
  t.bigSchema = "sf1";
  t.bigTable = "orders";
  t.bigRows = 1500000;
  t.longTable = "orders";
  t.longColumns = {"orderkey", "totalprice", "orderdate", "comment", "clerk"};
  t.longRows = kPreviewLimit;
  return t;
}

std::string quoteIdent(const std::string& name) { return "\"" + name + "\""; }

/// The SQL Power Query folds a navigator selection into: quoted, fully qualified, optional limit.
std::string selectSql(const std::vector<std::string>& columns, const std::string& cat, const std::string& schema,
                      const std::string& table, int limit) {
  std::string sql = "select ";
  for (size_t i = 0; i < columns.size(); ++i) sql += (i ? ", " : "") + quoteIdent(columns[i]);
  sql += " from " + quoteIdent(cat) + "." + quoteIdent(schema) + "." + quoteIdent(table);
  if (limit >= 0) sql += " limit " + std::to_string(limit);
  return sql;
}

std::vector<std::string> previewColumnNames(const Target& t) {
  std::vector<std::string> names;
  for (const ColumnSpec& c : t.preview) names.push_back(c.name);
  return names;
}

/// Text SQL types: a real server may run with UseUnicodeSqlCharacterTypes=0 in its connection
/// string, so only the mock (whose connection string the harness controls) is held to the W types.
bool sqlTypeMatches(const Target& t, SQLLEN actual, SQLSMALLINT expected) {
  if (actual == expected) return true;
  if (t.mock) return false;
  return (expected == SQL_WVARCHAR && actual == SQL_VARCHAR) || (expected == SQL_WCHAR && actual == SQL_CHAR) ||
         (expected == SQL_WLONGVARCHAR && actual == SQL_LONGVARCHAR);
}

bool isTextType(SQLSMALLINT t) {
  return t == SQL_WVARCHAR || t == SQL_VARCHAR || t == SQL_WCHAR || t == SQL_CHAR || t == SQL_WLONGVARCHAR ||
         t == SQL_LONGVARCHAR;
}

// ------------------------------------------------------------------------------------------------
// Expected mock values (mirror big_table_row() and TYPES_DEMO_ROWS of mock_trino.py)
// ------------------------------------------------------------------------------------------------

std::string expectedName(int64_t i) { return "name_" + std::to_string(i + 1); }
double expectedValue(int64_t i) { return static_cast<double>(i) * 0.5; }
std::string expectedAmount(int64_t i) {
  const long long a = static_cast<long long>(i) * 125;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld.%04lld", a / 100, (a % 100) * 100);
  return buf;
}
SQL_TIMESTAMP_STRUCT expectedTimestamp(int64_t i) {
  SQL_TIMESTAMP_STRUCT ts;
  std::memset(&ts, 0, sizeof ts);
  ts.year = 2024;
  ts.month = 1;
  ts.day = static_cast<SQLUSMALLINT>(1 + i / 86400);  // < 31 days for the 100k rows of the import
  ts.hour = static_cast<SQLUSMALLINT>((i % 86400) / 3600);
  ts.minute = static_cast<SQLUSMALLINT>((i % 3600) / 60);
  ts.second = static_cast<SQLUSMALLINT>(i % 60);
  return ts;
}
bool sameTimestamp(const SQL_TIMESTAMP_STRUCT& a, const SQL_TIMESTAMP_STRUCT& b) {
  return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute &&
         a.second == b.second && a.fraction == b.fraction;
}
std::string timestampText(const SQL_TIMESTAMP_STRUCT& t) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02u:%02u:%02u.%09lu", static_cast<int>(t.year),
                static_cast<unsigned>(t.month), static_cast<unsigned>(t.day), static_cast<unsigned>(t.hour),
                static_cast<unsigned>(t.minute), static_cast<unsigned>(t.second), static_cast<unsigned long>(t.fraction));
  return buf;
}

SQL_TIMESTAMP_STRUCT makeTimestamp(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s, unsigned long ns) {
  SQL_TIMESTAMP_STRUCT ts;
  std::memset(&ts, 0, sizeof ts);
  ts.year = static_cast<SQLSMALLINT>(y);
  ts.month = static_cast<SQLUSMALLINT>(mo);
  ts.day = static_cast<SQLUSMALLINT>(d);
  ts.hour = static_cast<SQLUSMALLINT>(h);
  ts.minute = static_cast<SQLUSMALLINT>(mi);
  ts.second = static_cast<SQLUSMALLINT>(s);
  ts.fraction = static_cast<SQLUINTEGER>(ns);
  return ts;
}

/// A date column bound as SQL_C_TYPE_TIMESTAMP: midnight of a TPC-H order date (1992-1998).
bool plausibleOrderDate(const SQL_TIMESTAMP_STRUCT& t) {
  return t.year >= 1992 && t.year <= 1998 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
         t.hour == 0 && t.minute == 0 && t.second == 0 && t.fraction == 0;
}

bool isOrderPriority(const std::string& s) {
  return s == "1-URGENT" || s == "2-HIGH" || s == "3-MEDIUM" || s == "4-NOT SPECIFIED" || s == "5-LOW";
}

// ------------------------------------------------------------------------------------------------
// Small ODBC helpers
// ------------------------------------------------------------------------------------------------

/// A nullable catalog-function argument (nullptr = SQL NULL pointer).
struct WArg {
  bool present;
  std::vector<SQLWCHAR> w;
  explicit WArg(const char* s) : present(s != nullptr) {
    if (s) w = toWide(s);
  }
  explicit WArg(const std::string& s) : present(true), w(toWide(s)) {}
  SQLWCHAR* ptr() { return present ? w.data() : nullptr; }
  SQLSMALLINT len() const { return present ? static_cast<SQLSMALLINT>(w.size() - 1) : 0; }
};

using Rows = std::vector<std::vector<std::string>>;

/// Every remaining row, each cell through SQLGetData(SQL_C_WCHAR) as Power BI reads catalog results.
Rows fetchAllWide(SQLHSTMT stmt) {
  Rows rows;
  const SQLSMALLINT n = numResultCols(stmt);
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetchScroll(stmt, SQL_FETCH_NEXT, 0);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) testfw::fail(__FILE__, __LINE__, "SQLFetchScroll failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    std::vector<std::string> row;
    for (SQLSMALLINT c = 1; c <= n; ++c) {
      bool isNull = false;
      std::string v = getWString(stmt, static_cast<SQLUSMALLINT>(c), &isNull, 1024);
      row.push_back(isNull ? std::string("<NULL>") : std::move(v));
    }
    rows.push_back(std::move(row));
    if (rows.size() > 1000000) testfw::fail(__FILE__, __LINE__, "catalog result does not end", true);
  }
  return rows;
}

void checkColumnNames(SQLHSTMT stmt, const std::vector<const char*>& expected, const char* function) {
  const std::vector<std::string> names = columnNames(stmt);
  testfw::State::get().checks++;
  if (names.size() != expected.size()) {
    testfw::fail(__FILE__, __LINE__,
                 std::string(function) + " has " + std::to_string(names.size()) + " columns, expected " +
                     std::to_string(expected.size()),
                 false);
    return;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    testfw::State::get().checks++;
    if (names[i] != expected[i]) {
      testfw::fail(__FILE__, __LINE__,
                   std::string(function) + " column " + std::to_string(i + 1) + " is \"" + names[i] + "\", expected \"" +
                       expected[i] + "\"",
                   false);
    }
  }
}

/// SQLGetData of a fixed-size C type; nullopt for NULL. Fatal on failure.
template <typename T>
std::optional<T> getFixed(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT cType, const char* what) {
  T v;
  std::memset(&v, 0, sizeof v);
  SQLLEN ind = 0;
  const SQLRETURN rc = Odbc.SQLGetData(stmt, col, cType, &v, static_cast<SQLLEN>(sizeof v), &ind);
  if (!SQL_SUCCEEDED(rc)) {
    testfw::fail(__FILE__, __LINE__, std::string("SQLGetData(") + what + ") failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return v;
}

std::optional<std::string> getText(SQLHSTMT stmt, SQLUSMALLINT col) {
  bool isNull = false;
  std::string v = getWString(stmt, col, &isNull, 512);
  if (isNull) return std::nullopt;
  return v;
}

/// Result of reading one column with repeated small SQLGetData(SQL_C_WCHAR) calls.
struct ChunkedText {
  bool isNull = false;
  std::string text;
  int calls = 0;
  SQLLEN firstIndicator = 0;
  size_t units = 0;  // SQLWCHAR units assembled
};

/// Reads column `col` of the current row in chunks of `bufUnits` SQLWCHAR units (terminator
/// included) until SQL_SUCCESS, verifying the 01004 / indicator / terminator protocol of every
/// call, then confirms the exhausted column answers SQL_NO_DATA.
ChunkedText getDataChunked(SQLHSTMT stmt, SQLUSMALLINT col, size_t bufUnits) {
  ChunkedText r;
  std::vector<SQLWCHAR> assembled;
  std::vector<SQLWCHAR> buf(bufUnits);
  for (;;) {
    // Poison the buffer so the terminator the driver writes is observable.
    for (SQLWCHAR& u : buf) u = static_cast<SQLWCHAR>(0x7777);
    SQLLEN ind = 0;
    const SQLRETURN rc =
        Odbc.SQLGetData(stmt, col, SQL_C_WCHAR, buf.data(), static_cast<SQLLEN>(bufUnits) * kUnit, &ind);
    if (rc == SQL_NO_DATA) {
      testfw::fail(__FILE__, __LINE__, "SQLGetData returned SQL_NO_DATA before the value was complete", false);
      break;
    }
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__,
                   "SQLGetData(col " + std::to_string(col) + ", SQL_C_WCHAR) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    ++r.calls;
    if (r.calls == 1) r.firstIndicator = ind;
    if (ind == SQL_NULL_DATA) {
      CHECK_EQ(r.calls, 1);  // NULL is reported by the first call, never after data
      r.isNull = true;
      return r;
    }
    size_t n = 0;
    if (rc == SQL_SUCCESS_WITH_INFO) {
      // 01004: the buffer holds as many whole characters as fit before the terminator.
      ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt, "01004");
      while (n < bufUnits && buf[n] != 0) ++n;
      CHECK(n < bufUnits);  // NUL-terminated
      CHECK(n > 0);         // progress on every truncated call
      CHECK(ind == SQL_NO_TOTAL || ind > static_cast<SQLLEN>(n) * kUnit);
    } else {
      REQUIRE(ind != SQL_NO_TOTAL);
      REQUIRE(ind >= 0 && ind % kUnit == 0);
      n = static_cast<size_t>(ind / kUnit);
      REQUIRE(n < bufUnits);
      CHECK(buf[n] == 0);
    }
    assembled.insert(assembled.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    if (rc == SQL_SUCCESS) break;
    REQUIRE(r.calls < 100000);
  }
  r.units = assembled.size();
  r.text = fromWide(assembled.data(), static_cast<SQLLEN>(assembled.size()));
  // The first indicator announced the complete length in bytes (drivers may say SQL_NO_TOTAL).
  CHECK(r.firstIndicator == SQL_NO_TOTAL || r.firstIndicator == static_cast<SQLLEN>(r.units) * kUnit);
  SQLWCHAR again[4] = {0, 0, 0, 0};
  SQLLEN ind = 0;
  CHECK(Odbc.SQLGetData(stmt, col, SQL_C_WCHAR, again, static_cast<SQLLEN>(sizeof again), &ind) == SQL_NO_DATA);
  return r;
}

double msSince(double t0) { return (nowSeconds() - t0) * 1000.0; }

// ------------------------------------------------------------------------------------------------
// Block-cursor buffers shared by the preview and the import (column-wise binding)
// ------------------------------------------------------------------------------------------------

struct RowsetBuffers {
  explicit RowsetBuffers(SQLULEN n)
      : keys(n), keyInd(n), text(n * kTextUnits), textInd(n), dbl(n), dblInd(n), ts(n), tsInd(n),
        text2(n * kTextUnits), text2Ind(n), status(n, 0xFFFF) {}
  std::vector<SQLBIGINT> keys;
  std::vector<SQLLEN> keyInd;
  std::vector<SQLWCHAR> text;
  std::vector<SQLLEN> textInd;
  std::vector<SQLDOUBLE> dbl;
  std::vector<SQLLEN> dblInd;
  std::vector<SQL_TIMESTAMP_STRUCT> ts;
  std::vector<SQLLEN> tsInd;
  std::vector<SQLWCHAR> text2;
  std::vector<SQLLEN> text2Ind;
  std::vector<SQLUSMALLINT> status;
  SQLULEN fetched = 0;
};

/// Text of bound wide cell `i`; `ok` cleared when NULL, truncated or unterminated.
std::string wideCell(const std::vector<SQLWCHAR>& buf, const std::vector<SQLLEN>& ind, size_t i, bool& ok) {
  const SQLLEN len = ind[i];
  if (len == SQL_NULL_DATA || len < 0 || len % kUnit != 0 || len >= static_cast<SQLLEN>(kTextUnits) * kUnit) {
    ok = false;
    return std::string();
  }
  const SQLWCHAR* cell = &buf[i * kTextUnits];
  const size_t units = static_cast<size_t>(len / kUnit);
  if (cell[units] != 0) ok = false;
  return fromWide(cell, static_cast<SQLLEN>(units));
}

struct FetchStats {
  int64_t rows = 0;
  int rowsets = 0;
  int64_t keySum = 0;
  int badRows = 0;
  double seconds = 0;
};

/// Verifies bound row `i` of the current rowset (0-based result row `row0`). `full` compares every
/// column; otherwise only the key and indicators (the sparse mode keeps the import loop cheap).
bool checkBoundRow(const Target& t, RowsetBuffers& b, size_t i, int64_t row0, bool full, std::string& why) {
  if (b.status[i] != SQL_ROW_SUCCESS) {
    why = "row status " + std::to_string(b.status[i]);
    return false;
  }
  if (b.keyInd[i] != static_cast<SQLLEN>(sizeof(SQLBIGINT))) {
    why = "key indicator " + std::to_string(static_cast<long long>(b.keyInd[i]));
    return false;
  }
  if (t.mock ? b.keys[i] != row0 + 1 : b.keys[i] <= 0) {
    why = "key " + std::to_string(static_cast<long long>(b.keys[i]));
    return false;
  }
  if (b.dblInd[i] != static_cast<SQLLEN>(sizeof(SQLDOUBLE)) ||
      b.tsInd[i] != static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT))) {
    why = "fixed-size indicator mismatch";
    return false;
  }
  if (!full) return true;
  bool ok = true;
  const std::string text = wideCell(b.text, b.textInd, i, ok);
  const std::string text2 = wideCell(b.text2, b.text2Ind, i, ok);
  if (!ok) {
    why = "text cell NULL, truncated or unterminated";
    return false;
  }
  if (t.mock) {
    const SQL_TIMESTAMP_STRUCT ts = expectedTimestamp(row0);
    if (text != expectedName(row0)) why = "name \"" + text + "\"";
    else if (b.textInd[i] != wideLength(expectedName(row0)) * kUnit) why = "name indicator";
    else if (b.dbl[i] != expectedValue(row0)) why = "value " + std::to_string(b.dbl[i]);
    else if (!sameTimestamp(b.ts[i], ts)) why = "ts " + timestampText(b.ts[i]) + " expected " + timestampText(ts);
    else if (text2 != expectedAmount(row0)) why = "amount \"" + text2 + "\" expected \"" + expectedAmount(row0) + "\"";
  } else {
    if (text.empty() || text.size() > 79) why = "comment \"" + text + "\"";
    else if (!(b.dbl[i] > 0.0)) why = "totalprice " + std::to_string(b.dbl[i]);
    else if (!plausibleOrderDate(b.ts[i])) why = "orderdate " + timestampText(b.ts[i]);
    else if (!isOrderPriority(text2)) why = "orderpriority \"" + text2 + "\"";
  }
  return why.empty();
}

/// SQLFetchScroll(SQL_FETCH_NEXT) until SQL_NO_DATA, checking the rows-fetched output, the status
/// array (including SQL_ROW_NOROW past a short rowset) and the bound values.
FetchStats fetchRowsets(SQLHSTMT stmt, const Target& t, RowsetBuffers& b, bool fullCheck) {
  FetchStats st;
  const double t0 = nowSeconds();
  for (;;) {
    for (SQLUSMALLINT& s : b.status) s = 0xFFFF;
    b.fetched = 0xDEAD;
    const SQLRETURN rc = Odbc.SQLFetchScroll(stmt, SQL_FETCH_NEXT, 0);
    if (rc == SQL_NO_DATA) break;
    if (rc != SQL_SUCCESS) {
      // SQL_SUCCESS_WITH_INFO would mean a truncated cell: every bound value must fit.
      testfw::fail(__FILE__, __LINE__,
                   std::string("SQLFetchScroll returned ") + rcName(rc) + ": " + diagText(SQL_HANDLE_STMT, stmt),
                   !SQL_SUCCEEDED(rc));
    }
    ++st.rowsets;
    REQUIRE(b.fetched >= 1 && b.fetched <= kRowset);
    for (size_t i = 0; i < static_cast<size_t>(b.fetched); ++i) {
      const int64_t row0 = st.rows + static_cast<int64_t>(i);
      st.keySum += b.keys[i];
      std::string why;
      const bool full = fullCheck || row0 % 997 == 0 || i + 1 == static_cast<size_t>(b.fetched);
      if (!checkBoundRow(t, b, i, row0, full, why) && st.badRows++ < 5) {
        testfw::fail(__FILE__, __LINE__, "row " + std::to_string(row0 + 1) + ": " + why, false);
      }
    }
    for (size_t i = static_cast<size_t>(b.fetched); i < b.status.size(); ++i) {
      if (b.status[i] != SQL_ROW_NOROW && st.badRows++ < 5) {
        testfw::fail(__FILE__, __LINE__,
                     "status[" + std::to_string(i) + "] past the rowset is " + std::to_string(b.status[i]), false);
      }
    }
    st.rows += static_cast<int64_t>(b.fetched);
  }
  st.seconds = nowSeconds() - t0;
  CHECK_EQ(b.fetched, static_cast<SQLULEN>(0));  // SQL_NO_DATA reports zero rows fetched
  CHECK_EQ(st.badRows, 0);
  return st;
}

// ------------------------------------------------------------------------------------------------
// Phases
// ------------------------------------------------------------------------------------------------

enum class InfoKind { Text, U16, U32 };
enum class Expect { Any, Exact, Bits };

struct InfoProbe {
  SQLUSMALLINT id;
  const char* name;
  InfoKind kind;
  Expect expect;
  const char* text;    // Exact text
  SQLUINTEGER value;   // Exact value or required bits
};

#define PB_TEXT(id, expected) InfoProbe{id, #id, InfoKind::Text, Expect::Exact, expected, 0}
#define PB_TEXT_ANY(id) InfoProbe{id, #id, InfoKind::Text, Expect::Any, nullptr, 0}
#define PB_U16(id, v) InfoProbe{id, #id, InfoKind::U16, Expect::Exact, nullptr, static_cast<SQLUINTEGER>(v)}
#define PB_U16_ANY(id) InfoProbe{id, #id, InfoKind::U16, Expect::Any, nullptr, 0}
#define PB_U32(id, v) InfoProbe{id, #id, InfoKind::U32, Expect::Exact, nullptr, static_cast<SQLUINTEGER>(v)}
#define PB_U32_BITS(id, v) InfoProbe{id, #id, InfoKind::U32, Expect::Bits, nullptr, static_cast<SQLUINTEGER>(v)}
#define PB_U32_ANY(id) InfoProbe{id, #id, InfoKind::U32, Expect::Any, nullptr, 0}

bool isDriverVersion(const std::string& v) {  // ##.##.####
  if (v.size() != 10 || v[2] != '.' || v[5] != '.') return false;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i != 2 && i != 5 && (v[i] < '0' || v[i] > '9')) return false;
  }
  return true;
}

/// The SQLGetInfoW probes the Power Query ODBC connector runs right after connecting. Returns the
/// number of probes issued.
int probeGetInfo(SQLHDBC dbc, bool mock) {
  const SQLUINTEGER cvtNumeric = SQL_CVT_VARCHAR | SQL_CVT_BIGINT | SQL_CVT_DOUBLE | SQL_CVT_INTEGER;
  const SQLUINTEGER cvtChar = SQL_CVT_VARCHAR | SQL_CVT_WVARCHAR | SQL_CVT_INTEGER | SQL_CVT_BIGINT | SQL_CVT_DOUBLE;
  const InfoProbe probes[] = {
      PB_TEXT_ANY(SQL_DRIVER_NAME),
      PB_TEXT_ANY(SQL_DRIVER_VER),
      PB_TEXT(SQL_DRIVER_ODBC_VER, "03.80"),
      PB_TEXT(SQL_DBMS_NAME, "Trino"),
      PB_TEXT_ANY(SQL_DBMS_VER),
      PB_TEXT_ANY(SQL_USER_NAME),
      PB_TEXT_ANY(SQL_DATABASE_NAME),
      PB_TEXT(SQL_IDENTIFIER_QUOTE_CHAR, "\""),
      PB_TEXT(SQL_CATALOG_NAME, "Y"),
      PB_TEXT(SQL_CATALOG_NAME_SEPARATOR, "."),
      PB_TEXT(SQL_CATALOG_TERM, "catalog"),
      PB_TEXT(SQL_SCHEMA_TERM, "schema"),
      PB_TEXT(SQL_TABLE_TERM, "table"),
      PB_TEXT(SQL_SEARCH_PATTERN_ESCAPE, "\\"),
      PB_TEXT(SQL_LIKE_ESCAPE_CLAUSE, "Y"),
      PB_TEXT(SQL_COLUMN_ALIAS, "Y"),
      PB_TEXT(SQL_EXPRESSIONS_IN_ORDERBY, "Y"),
      PB_TEXT(SQL_ORDER_BY_COLUMNS_IN_SELECT, "N"),
      PB_TEXT(SQL_SPECIAL_CHARACTERS, ""),
      PB_TEXT(SQL_DATA_SOURCE_READ_ONLY, "N"),
      PB_U16(SQL_CATALOG_LOCATION, SQL_CL_START),
      PB_U16(SQL_IDENTIFIER_CASE, SQL_IC_LOWER),
      PB_U16(SQL_QUOTED_IDENTIFIER_CASE, SQL_IC_SENSITIVE),
      PB_U16(SQL_GROUP_BY, SQL_GB_GROUP_BY_CONTAINS_SELECT),
      PB_U16(SQL_CONCAT_NULL_BEHAVIOR, SQL_CB_NULL),
      PB_U16(SQL_NULL_COLLATION, SQL_NC_END),
      PB_U16(SQL_CORRELATION_NAME, SQL_CN_ANY),
      PB_U16(SQL_TXN_CAPABLE, SQL_TC_NONE),
      PB_U16(SQL_MAX_IDENTIFIER_LEN, 128),
      PB_U16_ANY(SQL_MAX_COLUMN_NAME_LEN),
      PB_U16_ANY(SQL_MAX_TABLE_NAME_LEN),
      PB_U16_ANY(SQL_MAX_SCHEMA_NAME_LEN),
      PB_U16_ANY(SQL_MAX_CATALOG_NAME_LEN),
      PB_U16(SQL_MAX_COLUMNS_IN_SELECT, 0),
      PB_U32_ANY(SQL_MAX_ROW_SIZE),
      PB_U32_ANY(SQL_MAX_STATEMENT_LEN),
      PB_U32_ANY(SQL_MAX_CHAR_LITERAL_LEN),
      PB_U32_BITS(SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK | SQL_GD_BOUND),
      PB_U32(SQL_SQL_CONFORMANCE, SQL_SC_SQL92_ENTRY),
      PB_U32(SQL_ODBC_INTERFACE_CONFORMANCE, SQL_OIC_CORE),
      PB_U32(SQL_SCROLL_OPTIONS, SQL_SO_FORWARD_ONLY),
      PB_U32_BITS(SQL_SQL92_PREDICATES, SQL_SP_LIKE | SQL_SP_IN | SQL_SP_BETWEEN | SQL_SP_EXISTS | SQL_SP_ISNULL),
      PB_U32_BITS(SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_INNER_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_CROSS_JOIN),
      PB_U32_BITS(SQL_SQL92_VALUE_EXPRESSIONS, SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF),
      PB_U32_ANY(SQL_SQL92_NUMERIC_VALUE_FUNCTIONS),
      PB_U32_ANY(SQL_SQL92_STRING_FUNCTIONS),
      PB_U32_ANY(SQL_SQL92_DATETIME_FUNCTIONS),
      PB_U32_ANY(SQL_SQL92_ROW_VALUE_CONSTRUCTOR),
      PB_U32_BITS(SQL_CONVERT_FUNCTIONS, SQL_FN_CVT_CAST | SQL_FN_CVT_CONVERT),
      PB_U32_BITS(SQL_CONVERT_BIGINT, cvtNumeric),
      PB_U32_BITS(SQL_CONVERT_INTEGER, cvtNumeric),
      PB_U32_BITS(SQL_CONVERT_DOUBLE, cvtNumeric),
      PB_U32_BITS(SQL_CONVERT_DECIMAL, cvtNumeric),
      PB_U32_BITS(SQL_CONVERT_VARCHAR, cvtChar),
      PB_U32_BITS(SQL_CONVERT_WVARCHAR, cvtChar),
      PB_U32_BITS(SQL_CONVERT_TIMESTAMP, SQL_CVT_VARCHAR | SQL_CVT_TIMESTAMP | SQL_CVT_DATE),
      PB_U32_BITS(SQL_STRING_FUNCTIONS, SQL_FN_STR_CONCAT | SQL_FN_STR_LCASE | SQL_FN_STR_UCASE | SQL_FN_STR_SUBSTRING |
                                            SQL_FN_STR_LENGTH | SQL_FN_STR_LTRIM | SQL_FN_STR_RTRIM | SQL_FN_STR_REPLACE),
      PB_U32_BITS(SQL_NUMERIC_FUNCTIONS, SQL_FN_NUM_ABS | SQL_FN_NUM_MOD | SQL_FN_NUM_ROUND | SQL_FN_NUM_FLOOR |
                                             SQL_FN_NUM_CEILING | SQL_FN_NUM_SQRT | SQL_FN_NUM_POWER),
      PB_U32_BITS(SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_CURDATE | SQL_FN_TD_NOW | SQL_FN_TD_YEAR | SQL_FN_TD_MONTH |
                                              SQL_FN_TD_EXTRACT | SQL_FN_TD_TIMESTAMPADD | SQL_FN_TD_TIMESTAMPDIFF),
      PB_U32_BITS(SQL_SYSTEM_FUNCTIONS, SQL_FN_SYS_DBNAME | SQL_FN_SYS_USERNAME | SQL_FN_SYS_IFNULL),
      PB_U32_BITS(SQL_AGGREGATE_FUNCTIONS, SQL_AF_COUNT | SQL_AF_SUM | SQL_AF_MIN | SQL_AF_MAX | SQL_AF_AVG | SQL_AF_DISTINCT),
      PB_U32_BITS(SQL_OJ_CAPABILITIES, SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NESTED),
  };
  int issued = 0;
  for (const InfoProbe& p : probes) {
    ++issued;
    SQLRETURN rc = SQL_ERROR;
    std::string text;
    SQLUINTEGER value = 0;
    if (p.kind == InfoKind::Text) text = getInfoString(dbc, p.id, &rc);
    else if (p.kind == InfoKind::U16) value = getInfoUSmall(dbc, p.id, &rc);
    else value = getInfoUInt(dbc, p.id, &rc);
    testfw::State::get().checks++;
    if (rc != SQL_SUCCESS) {  // every probe fits its buffer: no 01004 either
      testfw::fail(__FILE__, __LINE__,
                   std::string("SQLGetInfoW(") + p.name + ") returned " + rcName(rc) + ": " + diagText(SQL_HANDLE_DBC, dbc),
                   false);
      continue;
    }
    std::string mismatch;
    if (p.expect == Expect::Exact && p.kind == InfoKind::Text && text != p.text) {
      mismatch = "\"" + text + "\", expected \"" + p.text + "\"";
    } else if (p.expect == Expect::Exact && p.kind != InfoKind::Text && value != p.value) {
      mismatch = std::to_string(value) + ", expected " + std::to_string(p.value);
    } else if (p.expect == Expect::Bits && (value & p.value) != p.value) {
      mismatch = std::to_string(value) + ", missing bits " + std::to_string(p.value & ~value);
    }
    if (!mismatch.empty()) testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + p.name + ") = " + mismatch, false);
  }
  // Values Power BI parses rather than compares.
  CHECK(getInfoString(dbc, SQL_DRIVER_NAME).find("trinoodbc") != std::string::npos);
  CHECK(isDriverVersion(getInfoString(dbc, SQL_DRIVER_VER)));
  CHECK(!getInfoString(dbc, SQL_DBMS_VER).empty());
  for (const SQLUSMALLINT id : {SQL_MAX_COLUMN_NAME_LEN, SQL_MAX_TABLE_NAME_LEN, SQL_MAX_SCHEMA_NAME_LEN,
                                SQL_MAX_CATALOG_NAME_LEN}) {
    const SQLUSMALLINT v = getInfoUSmall(dbc, id);
    CHECK(v == 0 || v >= 128);  // 0 = unlimited; anything shorter would make Power BI truncate names
  }
  if (mock) {
    CHECK(getInfoString(dbc, SQL_DBMS_VER).find("mock") != std::string::npos);
    CHECK_EQ(getInfoString(dbc, SQL_USER_NAME), std::string("test"));
    CHECK_EQ(getInfoString(dbc, SQL_DATABASE_NAME), std::string("mock"));
  }
  return issued;
}

#undef PB_TEXT
#undef PB_TEXT_ANY
#undef PB_U16
#undef PB_U16_ANY
#undef PB_U32
#undef PB_U32_BITS
#undef PB_U32_ANY

void checkGetFunctions(SQLHDBC dbc) {
  SQLUSMALLINT bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE];
  std::memset(bitmap, 0, sizeof bitmap);
  ODBC_REQUIRE(Odbc.SQLGetFunctions(dbc, SQL_API_ODBC3_ALL_FUNCTIONS, bitmap), SQL_HANDLE_DBC, dbc);
  // Everything the connector calls during the flow below, plus what it uses for folding/params.
  const SQLUSMALLINT used[] = {
      SQL_API_SQLALLOCHANDLE,    SQL_API_SQLFREEHANDLE,     SQL_API_SQLDRIVERCONNECT, SQL_API_SQLDISCONNECT,
      SQL_API_SQLGETINFO,        SQL_API_SQLGETFUNCTIONS,   SQL_API_SQLGETTYPEINFO,   SQL_API_SQLTABLES,
      SQL_API_SQLCOLUMNS,        SQL_API_SQLPRIMARYKEYS,    SQL_API_SQLFOREIGNKEYS,   SQL_API_SQLSTATISTICS,
      SQL_API_SQLPREPARE,        SQL_API_SQLEXECUTE,        SQL_API_SQLEXECDIRECT,    SQL_API_SQLNUMRESULTCOLS,
      SQL_API_SQLDESCRIBECOL,    SQL_API_SQLCOLATTRIBUTE,   SQL_API_SQLBINDCOL,       SQL_API_SQLFETCHSCROLL,
      SQL_API_SQLFETCH,          SQL_API_SQLGETDATA,        SQL_API_SQLSETSTMTATTR,   SQL_API_SQLGETSTMTATTR,
      SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR, SQL_API_SQLSETENVATTR,    SQL_API_SQLGETENVATTR,
      SQL_API_SQLFREESTMT,       SQL_API_SQLCLOSECURSOR,    SQL_API_SQLMORERESULTS,   SQL_API_SQLROWCOUNT,
      SQL_API_SQLGETDIAGREC,     SQL_API_SQLGETDIAGFIELD,   SQL_API_SQLBINDPARAMETER, SQL_API_SQLNUMPARAMS,
      SQL_API_SQLDESCRIBEPARAM,  SQL_API_SQLCANCEL,         SQL_API_SQLENDTRAN,       SQL_API_SQLNATIVESQL,
  };
  for (const SQLUSMALLINT id : used) {
    testfw::State::get().checks++;
    if (!SQL_FUNC_EXISTS(bitmap, id)) {
      testfw::fail(__FILE__, __LINE__, "SQLGetFunctions(ALL) lacks function id " + std::to_string(id), false);
    }
  }
}

/// SQLGetTypeInfoW(SQL_ALL_TYPES) read row by row with typed SQLGetData calls, as Power BI does to
/// build its type map. Returns the number of rows.
size_t readTypeInfo(SQLHDBC dbc) {
  Stmt stmt(dbc);
  ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(stmt.h, SQL_ALL_TYPES), SQL_HANDLE_STMT, stmt.h);
  checkColumnNames(stmt.h,
                   {"TYPE_NAME", "DATA_TYPE", "COLUMN_SIZE", "LITERAL_PREFIX", "LITERAL_SUFFIX", "CREATE_PARAMS",
                    "NULLABLE", "CASE_SENSITIVE", "SEARCHABLE", "UNSIGNED_ATTRIBUTE", "FIXED_PREC_SCALE",
                    "AUTO_UNIQUE_VALUE", "LOCAL_TYPE_NAME", "MINIMUM_SCALE", "MAXIMUM_SCALE", "SQL_DATA_TYPE",
                    "SQL_DATETIME_SUB", "NUM_PREC_RADIX", "INTERVAL_PRECISION"},
                   "SQLGetTypeInfo");
  size_t rows = 0;
  SQLSMALLINT previousType = std::numeric_limits<SQLSMALLINT>::min();
  bool ordered = true;
  bool sawBigint = false, sawDouble = false, sawVarchar = false, sawTimestamp = false, sawDecimal = false,
       sawDate = false, sawBoolean = false;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++rows;
    REQUIRE(rows < 1000);
    const std::optional<std::string> typeName = getText(stmt.h, 1);
    const std::optional<SQLSMALLINT> dataType = getFixed<SQLSMALLINT>(stmt.h, 2, SQL_C_SSHORT, "DATA_TYPE");
    const std::optional<SQLINTEGER> columnSize = getFixed<SQLINTEGER>(stmt.h, 3, SQL_C_SLONG, "COLUMN_SIZE");
    const std::optional<std::string> prefix = getText(stmt.h, 4);
    const std::optional<std::string> suffix = getText(stmt.h, 5);
    const std::optional<std::string> createParams = getText(stmt.h, 6);
    const std::optional<SQLSMALLINT> nullable = getFixed<SQLSMALLINT>(stmt.h, 7, SQL_C_SSHORT, "NULLABLE");
    const std::optional<SQLSMALLINT> caseSensitive = getFixed<SQLSMALLINT>(stmt.h, 8, SQL_C_SSHORT, "CASE_SENSITIVE");
    const std::optional<SQLSMALLINT> searchable = getFixed<SQLSMALLINT>(stmt.h, 9, SQL_C_SSHORT, "SEARCHABLE");
    const std::optional<SQLSMALLINT> unsignedAttr = getFixed<SQLSMALLINT>(stmt.h, 10, SQL_C_SSHORT, "UNSIGNED_ATTRIBUTE");
    const std::optional<SQLSMALLINT> fixedPrecScale = getFixed<SQLSMALLINT>(stmt.h, 11, SQL_C_SSHORT, "FIXED_PREC_SCALE");
    const std::optional<SQLSMALLINT> autoUnique = getFixed<SQLSMALLINT>(stmt.h, 12, SQL_C_SSHORT, "AUTO_UNIQUE_VALUE");
    const std::optional<std::string> localName = getText(stmt.h, 13);
    const std::optional<SQLSMALLINT> minScale = getFixed<SQLSMALLINT>(stmt.h, 14, SQL_C_SSHORT, "MINIMUM_SCALE");
    const std::optional<SQLSMALLINT> maxScale = getFixed<SQLSMALLINT>(stmt.h, 15, SQL_C_SSHORT, "MAXIMUM_SCALE");
    const std::optional<SQLSMALLINT> sqlDataType = getFixed<SQLSMALLINT>(stmt.h, 16, SQL_C_SSHORT, "SQL_DATA_TYPE");
    const std::optional<SQLSMALLINT> datetimeSub = getFixed<SQLSMALLINT>(stmt.h, 17, SQL_C_SSHORT, "SQL_DATETIME_SUB");
    const std::optional<SQLINTEGER> radix = getFixed<SQLINTEGER>(stmt.h, 18, SQL_C_SLONG, "NUM_PREC_RADIX");
    const std::optional<SQLSMALLINT> intervalPrecision = getFixed<SQLSMALLINT>(stmt.h, 19, SQL_C_SSHORT, "INTERVAL_PRECISION");
    (void)caseSensitive;
    (void)fixedPrecScale;
    (void)autoUnique;
    (void)localName;
    (void)intervalPrecision;
    (void)suffix;

    // Columns the spec declares NOT NULL.
    REQUIRE(typeName.has_value() && !typeName->empty());
    REQUIRE(dataType.has_value());
    CHECK(nullable.has_value() && *nullable == SQL_NULLABLE);
    CHECK(searchable.has_value());
    CHECK(sqlDataType.has_value());
    if (*dataType < previousType) ordered = false;
    previousType = *dataType;
    const std::string& name = *typeName;
    if (*dataType == SQL_BIGINT) {
      sawBigint = true;
      CHECK(columnSize.has_value() && *columnSize == 19);
      CHECK(!prefix.has_value());  // numbers have no literal prefix
      CHECK(unsignedAttr.has_value() && *unsignedAttr == SQL_FALSE);
      CHECK(radix.has_value() && *radix == 10);
    }
    if (*dataType == SQL_DOUBLE) sawDouble = true;
    if (name == "boolean") sawBoolean = true;
    if (name == "varchar" && (*dataType == SQL_WVARCHAR || *dataType == SQL_VARCHAR)) {
      sawVarchar = true;
      CHECK(prefix.has_value() && *prefix == "'");
      CHECK(createParams.has_value() && *createParams == "length");
      CHECK(!radix.has_value());
    }
    if (*dataType == SQL_TYPE_TIMESTAMP) {
      sawTimestamp = true;
      CHECK(sqlDataType.has_value() && *sqlDataType == SQL_DATETIME);
      CHECK(datetimeSub.has_value() && *datetimeSub == SQL_CODE_TIMESTAMP);
    }
    if (*dataType == SQL_TYPE_DATE) {
      sawDate = true;
      CHECK(datetimeSub.has_value() && *datetimeSub == SQL_CODE_DATE);
    }
    if (name == "decimal") {
      sawDecimal = true;
      CHECK_EQ(*dataType, static_cast<SQLSMALLINT>(SQL_DECIMAL));
      CHECK(minScale.has_value() && *minScale == 0);
      CHECK(maxScale.has_value() && *maxScale == 38);
      CHECK(createParams.has_value() && *createParams == "precision,scale");
    }
  }
  CHECK(ordered);  // SQLGetTypeInfo is ordered by DATA_TYPE
  CHECK(rows >= 12);
  CHECK(sawBigint);
  CHECK(sawDouble);
  CHECK(sawVarchar);
  CHECK(sawTimestamp);
  CHECK(sawDecimal);
  CHECK(sawDate);
  CHECK(sawBoolean);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  return rows;
}

/// The navigator: catalogs, tables of the catalog, tables of the schema, then the selected table's
/// columns, keys and indexes. Returns false (after skipping) when the scenario's catalog is absent.
bool browseNavigator(SQLHDBC dbc, const Target& t, Rows& columnsOut) {
  // SQLTablesW(SQL_ALL_CATALOGS, "", "", NULL): the navigator's root nodes.
  {
    Stmt stmt(dbc);
    WArg cat(SQL_ALL_CATALOGS), empty1(""), empty2("");
    ODBC_REQUIRE(Odbc.SQLTablesW(stmt.h, cat.ptr(), cat.len(), empty1.ptr(), empty1.len(), empty2.ptr(), empty2.len(),
                                 nullptr, 0),
                 SQL_HANDLE_STMT, stmt.h);
    checkColumnNames(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"}, "SQLTables");
    const Rows rows = fetchAllWide(stmt.h);
    bool sawCatalog = false;
    for (const std::vector<std::string>& r : rows) {
      CHECK_EQ(r[1], std::string("<NULL>"));
      CHECK_EQ(r[2], std::string("<NULL>"));
      if (r[0] == t.catalog) sawCatalog = true;
    }
    if (t.mock) {
      REQUIRE_EQ(rows.size(), static_cast<size_t>(3));
      CHECK_EQ(rows[0][0], std::string("mock"));
      CHECK_EQ(rows[1][0], std::string("system"));
      CHECK_EQ(rows[2][0], std::string("tpch"));
    } else if (!sawCatalog) {
      skip("catalog '" + t.catalog + "' is not available on this server");
      return false;
    }
    CHECK(sawCatalog);
  }
  // SQLTablesW(catalog, NULL, NULL, NULL): expanding the catalog node.
  {
    Stmt stmt(dbc);
    WArg cat(t.catalog);
    ODBC_REQUIRE(Odbc.SQLTablesW(stmt.h, cat.ptr(), cat.len(), nullptr, 0, nullptr, 0, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
    const Rows rows = fetchAllWide(stmt.h);
    bool sawTable = false;
    for (const std::vector<std::string>& r : rows) {
      CHECK_EQ(r[0], t.catalog);
      CHECK(r[3] == "TABLE" || r[3] == "VIEW");
      if (r[1] == t.schema && r[2] == t.table) sawTable = true;
    }
    CHECK(sawTable);
    if (t.mock) {
      // Ordered by TABLE_TYPE, TABLE_CAT, TABLE_SCHEM, TABLE_NAME.
      REQUIRE_EQ(rows.size(), t.schemaTables.size());
      for (size_t i = 0; i < rows.size(); ++i) {
        CHECK_EQ(rows[i][1], t.schema);
        CHECK_EQ(rows[i][2], std::string(t.schemaTables[i].name));
        CHECK_EQ(rows[i][3], std::string(t.schemaTables[i].type));
      }
    }
  }
  // SQLTablesW(catalog, schema, "%", "TABLE,VIEW"): expanding the schema node.
  {
    Stmt stmt(dbc);
    WArg cat(t.catalog), schem(t.schema), pattern("%"), types("TABLE,VIEW");
    ODBC_REQUIRE(Odbc.SQLTablesW(stmt.h, cat.ptr(), cat.len(), schem.ptr(), schem.len(), pattern.ptr(), pattern.len(),
                                 types.ptr(), types.len()),
                 SQL_HANDLE_STMT, stmt.h);
    const Rows rows = fetchAllWide(stmt.h);
    REQUIRE_EQ(rows.size(), t.schemaTables.size());
    for (size_t i = 0; i < rows.size(); ++i) {
      CHECK_EQ(rows[i][0], t.catalog);
      CHECK_EQ(rows[i][1], t.schema);
      CHECK_EQ(rows[i][2], std::string(t.schemaTables[i].name));
      CHECK_EQ(rows[i][3], std::string(t.schemaTables[i].type));
      if (t.mock) CHECK_EQ(rows[i][4], std::string("<NULL>"));  // REMARKS
    }
  }
  WArg cat(t.catalog), schem(t.schema), table(t.table);
  // SQLColumnsW(catalog, schema, table, NULL): the preview's column list.
  {
    Stmt stmt(dbc);
    ODBC_REQUIRE(Odbc.SQLColumnsW(stmt.h, cat.ptr(), cat.len(), schem.ptr(), schem.len(), table.ptr(), table.len(), nullptr, 0),
                 SQL_HANDLE_STMT, stmt.h);
    REQUIRE(numResultCols(stmt.h) >= 18);
    const Rows rows = fetchAllWide(stmt.h);
    REQUIRE_EQ(rows.size(), t.tableColumns.size());
    for (size_t i = 0; i < rows.size(); ++i) {
      CHECK_EQ(rows[i][0], t.catalog);
      CHECK_EQ(rows[i][1], t.schema);
      CHECK_EQ(rows[i][2], t.table);
      CHECK_EQ(rows[i][3], t.tableColumns[i]);
      CHECK_EQ(rows[i][16], std::to_string(i + 1));  // ORDINAL_POSITION
    }
    for (const ColumnSpec& c : t.preview) {
      const std::vector<std::string>* row = nullptr;
      for (const std::vector<std::string>& r : rows) {
        if (r[3] == c.name) row = &r;
      }
      REQUIRE(row != nullptr);
      CHECK(sqlTypeMatches(t, std::atol((*row)[4].c_str()), c.sqlType));  // DATA_TYPE
      CHECK_EQ((*row)[5], std::string(c.typeName));                        // TYPE_NAME
      CHECK_EQ((*row)[6], std::to_string(c.size));                         // COLUMN_SIZE
      if ((*row)[8] != "<NULL>") CHECK_EQ((*row)[8], std::to_string(c.digits));  // DECIMAL_DIGITS
    }
    columnsOut = rows;
  }
  // SQLPrimaryKeysW / SQLForeignKeysW / SQLStatisticsW: relationship and index discovery. Trino has
  // neither, so these are empty but must have the spec layouts.
  {
    Stmt stmt(dbc);
    ODBC_REQUIRE(Odbc.SQLPrimaryKeysW(stmt.h, cat.ptr(), cat.len(), schem.ptr(), schem.len(), table.ptr(), table.len()),
                 SQL_HANDLE_STMT, stmt.h);
    checkColumnNames(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"},
                     "SQLPrimaryKeys");
    CHECK_EQ(fetchAllWide(stmt.h).size(), static_cast<size_t>(0));
  }
  {
    Stmt stmt(dbc);
    ODBC_REQUIRE(Odbc.SQLForeignKeysW(stmt.h, nullptr, 0, nullptr, 0, nullptr, 0, cat.ptr(), cat.len(), schem.ptr(),
                                      schem.len(), table.ptr(), table.len()),
                 SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(14));
    CHECK_EQ(fetchAllWide(stmt.h).size(), static_cast<size_t>(0));
  }
  {
    Stmt stmt(dbc);
    ODBC_REQUIRE(Odbc.SQLStatisticsW(stmt.h, cat.ptr(), cat.len(), schem.ptr(), schem.len(), table.ptr(), table.len(),
                                     SQL_INDEX_ALL, SQL_QUICK),
                 SQL_HANDLE_STMT, stmt.h);
    checkColumnNames(stmt.h,
                     {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "NON_UNIQUE", "INDEX_QUALIFIER", "INDEX_NAME", "TYPE",
                      "ORDINAL_POSITION", "COLUMN_NAME", "ASC_OR_DESC", "CARDINALITY", "PAGES", "FILTER_CONDITION"},
                     "SQLStatistics");
    CHECK_EQ(fetchAllWide(stmt.h).size(), static_cast<size_t>(0));
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  }
  return true;
}

/// SQLNumResultCols + SQLDescribeColW + SQLColAttributeW of every column of the prepared (not yet
/// executed) preview, against the scenario and against SQLColumns.
void checkPreparedMetadata(SQLHSTMT stmt, const Target& t, const Rows& columnRows) {
  REQUIRE_EQ(numResultCols(stmt), static_cast<SQLSMALLINT>(t.preview.size()));
  CHECK_EQ(colAttrNum(stmt, 0, SQL_DESC_COUNT), static_cast<SQLLEN>(t.preview.size()));
  for (size_t i = 0; i < t.preview.size(); ++i) {
    const ColumnSpec& e = t.preview[i];
    const SQLUSMALLINT col = static_cast<SQLUSMALLINT>(i + 1);
    const ColumnDesc d = describeCol(stmt, col);
    testfw::State::get().checks++;
    if (d.name != e.name || !sqlTypeMatches(t, d.type, e.sqlType) || d.size != e.size || d.digits != e.digits) {
      testfw::fail(__FILE__, __LINE__,
                   "SQLDescribeColW(" + std::to_string(col) + ") before SQLExecute = {" + d.name + ", " +
                       std::to_string(d.type) + ", " + std::to_string(static_cast<unsigned long long>(d.size)) + ", " +
                       std::to_string(d.digits) + "}, expected {" + e.name + ", " + std::to_string(e.sqlType) + ", " +
                       std::to_string(static_cast<unsigned long long>(e.size)) + ", " + std::to_string(e.digits) + "}",
                   false);
    }
    CHECK(d.nullable == SQL_NULLABLE || d.nullable == SQL_NULLABLE_UNKNOWN);
    // SQLColumns and the prepared statement must agree, or Power BI's schema cache goes stale.
    for (const std::vector<std::string>& r : columnRows) {
      if (r[3] != e.name) continue;
      CHECK_EQ(r[4], std::to_string(d.type));
      CHECK_EQ(r[6], std::to_string(static_cast<unsigned long long>(d.size)));
    }
    // The SQLColAttributeW fields the connector reads per column.
    SQLRETURN rc = SQL_ERROR;
    CHECK(sqlTypeMatches(t, colAttrNum(stmt, col, SQL_DESC_CONCISE_TYPE, &rc), e.sqlType));
    ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt);
    CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_CONCISE_TYPE), static_cast<SQLLEN>(d.type));
    CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_LENGTH), static_cast<SQLLEN>(e.size));
    CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_SCALE), static_cast<SQLLEN>(e.digits));
    CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_NULLABLE), static_cast<SQLLEN>(d.nullable));
    const SQLLEN octets = colAttrNum(stmt, col, SQL_DESC_OCTET_LENGTH, &rc);
    ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt);
    CHECK(octets > 0);
    const SQLLEN display = colAttrNum(stmt, col, SQL_DESC_DISPLAY_SIZE, &rc);
    ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt);
    CHECK(display > 0);
    const SQLLEN unsignedAttr = colAttrNum(stmt, col, SQL_DESC_UNSIGNED, &rc);
    ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt);
    if (e.sqlType == SQL_BIGINT || e.sqlType == SQL_DOUBLE || e.sqlType == SQL_DECIMAL) {
      CHECK_EQ(unsignedAttr, static_cast<SQLLEN>(SQL_FALSE));
    } else if (isTextType(e.sqlType)) {
      CHECK_EQ(unsignedAttr, static_cast<SQLLEN>(SQL_TRUE));
    }
    CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_UPDATABLE), static_cast<SQLLEN>(SQL_ATTR_READONLY));
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_NAME), std::string(e.name));
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_LABEL), std::string(e.name));
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_BASE_COLUMN_NAME), std::string(e.name));
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_TYPE_NAME), std::string(e.typeName));
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_TABLE_NAME), t.table);
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_SCHEMA_NAME), t.schema);
    CHECK_EQ(colAttrString(stmt, col, SQL_DESC_CATALOG_NAME), t.catalog);
    if (e.sqlType == SQL_TYPE_TIMESTAMP || e.sqlType == SQL_TYPE_DATE) {
      CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_TYPE), static_cast<SQLLEN>(SQL_DATETIME));
    }
    if (e.sqlType == SQL_DECIMAL) {
      CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_PRECISION), static_cast<SQLLEN>(e.size));
      CHECK_EQ(colAttrNum(stmt, col, SQL_DESC_NUM_PREC_RADIX), static_cast<SQLLEN>(10));
    }
  }
}

/// SQLBindCol of the five preview columns into the rowset buffers (column-wise binding).
void bindPreviewColumns(SQLHSTMT stmt, RowsetBuffers& b) {
  const SQLLEN textBytes = static_cast<SQLLEN>(kTextUnits) * kUnit;
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt, 1, SQL_C_SBIGINT, b.keys.data(), sizeof(SQLBIGINT), b.keyInd.data()),
               SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt, 2, SQL_C_WCHAR, b.text.data(), textBytes, b.textInd.data()), SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt, 3, SQL_C_DOUBLE, b.dbl.data(), sizeof(SQLDOUBLE), b.dblInd.data()), SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt, 4, SQL_C_TYPE_TIMESTAMP, b.ts.data(), sizeof(SQL_TIMESTAMP_STRUCT), b.tsInd.data()),
               SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt, 5, SQL_C_WCHAR, b.text2.data(), textBytes, b.text2Ind.data()), SQL_HANDLE_STMT, stmt);
}

/// SQLSetStmtAttrW(ROW_ARRAY_SIZE / ROWS_FETCHED_PTR / ROW_STATUS_PTR) and read-back.
void setBlockCursor(SQLHSTMT stmt, SQLULEN rowset, SQLULEN* fetched, SQLUSMALLINT* status) {
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(rowset), SQL_IS_UINTEGER), SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt, SQL_ATTR_ROWS_FETCHED_PTR, fetched, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt, SQL_ATTR_ROW_STATUS_PTR, status, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt);
  CHECK_EQ(getStmtAttrInt(stmt, SQL_ATTR_ROW_ARRAY_SIZE), rowset);
  SQLPOINTER p = nullptr;
  SQLINTEGER len = 0;
  ODBC_CHECK(Odbc.SQLGetStmtAttrW(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &p, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt);
  CHECK(p == static_cast<SQLPOINTER>(fetched));
}

struct LongRowExpect {
  bool allNull;
  int64_t bigint;
  double dbl;  // compared exactly; -inf for row 3
  SQL_TIMESTAMP_STRUCT ts;
  const char* text;
  const char* json;
};

/// Long-text phase: rowset of one, three bound columns, the two text columns via chunked
/// SQLGetData (the path Power BI takes for columns too wide to bind).
int64_t readLongText(SQLHDBC dbc, const Target& t, int& getDataCalls) {
  const LongRowExpect mockRows[] = {
      {false, std::numeric_limits<int64_t>::max(), 3.141592653589793, makeTimestamp(2024, 3, 10, 2, 30, 0, 123000000),
       "hello world", "{\"a\":1,\"b\":[1,2]}"},
      {true, 0, 0.0, makeTimestamp(0, 0, 0, 0, 0, 0, 0), nullptr, nullptr},
      {false, std::numeric_limits<int64_t>::min(), -std::numeric_limits<double>::infinity(),
       makeTimestamp(1970, 1, 1, 0, 0, 0, 0), kUnicodeText, "\"str\""},
  };
  Stmt stmt(dbc);
  std::vector<std::string> cols(t.longColumns.begin(), t.longColumns.end());
  ODBC_REQUIRE(execDirect(stmt.h, selectSql(cols, t.catalog, t.schema, t.longTable, kPreviewLimit)), SQL_HANDLE_STMT, stmt.h);
  REQUIRE_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(5));
  if (t.mock) {
    CHECK_EQ(describeCol(stmt.h, 4).type, static_cast<SQLSMALLINT>(SQL_WVARCHAR));
    CHECK_EQ(describeCol(stmt.h, 4).size, static_cast<SQLULEN>(65535));
    CHECK_EQ(describeCol(stmt.h, 5).type, static_cast<SQLSMALLINT>(SQL_WLONGVARCHAR));  // json
  }
  SQLBIGINT key = 0;
  SQLDOUBLE dbl = 0;
  SQL_TIMESTAMP_STRUCT ts;
  std::memset(&ts, 0, sizeof ts);
  SQLLEN keyInd = 0, dblInd = 0, tsInd = 0;
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, &key, sizeof key, &keyInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 2, SQL_C_DOUBLE, &dbl, sizeof dbl, &dblInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 3, SQL_C_TYPE_TIMESTAMP, &ts, sizeof ts, &tsInd), SQL_HANDLE_STMT, stmt.h);
  SQLULEN fetched = 0;
  SQLUSMALLINT status[1] = {0xFFFF};
  setBlockCursor(stmt.h, 1, &fetched, status);
  int64_t rows = 0;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    REQUIRE(rows < t.longRows);
    CHECK_EQ(fetched, static_cast<SQLULEN>(1));
    CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS));
    const ChunkedText text = getDataChunked(stmt.h, 4, kChunkUnits);
    const ChunkedText second = getDataChunked(stmt.h, 5, kChunkUnits);
    getDataCalls += text.calls + second.calls;
    if (t.mock) {
      const LongRowExpect& e = mockRows[rows];
      if (e.allNull) {
        CHECK_EQ(keyInd, static_cast<SQLLEN>(SQL_NULL_DATA));
        CHECK_EQ(dblInd, static_cast<SQLLEN>(SQL_NULL_DATA));
        CHECK_EQ(tsInd, static_cast<SQLLEN>(SQL_NULL_DATA));
        CHECK(text.isNull);
        CHECK(second.isNull);
      } else {
        CHECK_EQ(static_cast<int64_t>(key), e.bigint);
        CHECK(dbl == e.dbl);
        CHECK_EQ(tsInd, static_cast<SQLLEN>(sizeof ts));
        CHECK_EQ(timestampText(ts), timestampText(e.ts));
        CHECK(!text.isNull);
        CHECK_EQ(text.text, std::string(e.text));
        CHECK_EQ(static_cast<SQLLEN>(text.units), wideLength(e.text));
        // A 4-unit payload per call: the text needs at least ceil(units / 4) calls.
        CHECK(static_cast<size_t>(text.calls) >= (text.units + kChunkUnits - 2) / (kChunkUnits - 1));
        CHECK_EQ(second.text, std::string(e.json));
      }
    } else {
      CHECK(key > 0);
      CHECK(dbl > 0.0);
      CHECK(plausibleOrderDate(ts));
      CHECK(!text.isNull && !text.text.empty() && text.text.size() <= 79);
      CHECK(text.calls >= 1);
      CHECK(!second.isNull && second.text.compare(0, 6, "Clerk#") == 0);
    }
    ++rows;
  }
  CHECK_EQ(rows, t.longRows);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  return rows;
}

/// Exports the flow calls; a driver without one cannot serve Power BI at all.
void requireExports() {
  struct Needed {
    const char* name;
    bool present;
  };
  const Needed needed[] = {
      {"SQLAllocHandle", Odbc.SQLAllocHandle != nullptr},     {"SQLSetEnvAttr", Odbc.SQLSetEnvAttr != nullptr},
      {"SQLGetEnvAttr", Odbc.SQLGetEnvAttr != nullptr},       {"SQLSetConnectAttrW", Odbc.SQLSetConnectAttrW != nullptr},
      {"SQLGetConnectAttrW", Odbc.SQLGetConnectAttrW != nullptr}, {"SQLDriverConnectW", Odbc.SQLDriverConnectW != nullptr},
      {"SQLGetInfoW", Odbc.SQLGetInfoW != nullptr},           {"SQLGetFunctions", Odbc.SQLGetFunctions != nullptr},
      {"SQLGetTypeInfoW", Odbc.SQLGetTypeInfoW != nullptr},   {"SQLTablesW", Odbc.SQLTablesW != nullptr},
      {"SQLColumnsW", Odbc.SQLColumnsW != nullptr},           {"SQLPrimaryKeysW", Odbc.SQLPrimaryKeysW != nullptr},
      {"SQLForeignKeysW", Odbc.SQLForeignKeysW != nullptr},   {"SQLStatisticsW", Odbc.SQLStatisticsW != nullptr},
      {"SQLPrepareW", Odbc.SQLPrepareW != nullptr},           {"SQLNumResultCols", Odbc.SQLNumResultCols != nullptr},
      {"SQLDescribeColW", Odbc.SQLDescribeColW != nullptr},   {"SQLColAttributeW", Odbc.SQLColAttributeW != nullptr},
      {"SQLExecute", Odbc.SQLExecute != nullptr},             {"SQLExecDirectW", Odbc.SQLExecDirectW != nullptr},
      {"SQLBindCol", Odbc.SQLBindCol != nullptr},             {"SQLSetStmtAttrW", Odbc.SQLSetStmtAttrW != nullptr},
      {"SQLGetStmtAttrW", Odbc.SQLGetStmtAttrW != nullptr},   {"SQLFetchScroll", Odbc.SQLFetchScroll != nullptr},
      {"SQLGetData", Odbc.SQLGetData != nullptr},             {"SQLRowCount", Odbc.SQLRowCount != nullptr},
      {"SQLMoreResults", Odbc.SQLMoreResults != nullptr},     {"SQLFreeStmt", Odbc.SQLFreeStmt != nullptr},
      {"SQLFreeHandle", Odbc.SQLFreeHandle != nullptr},       {"SQLDisconnect", Odbc.SQLDisconnect != nullptr},
  };
  std::string missing;
  for (const Needed& n : needed) {
    if (!n.present) missing += (missing.empty() ? "" : ", ") + std::string(n.name);
  }
  testfw::State::get().checks++;
  if (!missing.empty()) testfw::fail(__FILE__, __LINE__, "driver lacks exports used by Power BI: " + missing, true);
}

}  // namespace

TEST_CASE("powerbi_flow/desktop_connect_navigate_preview_import") {
  requireExports();
  const double flowStart = nowSeconds();
  std::string phases;
  auto phaseDone = [&phases](const char* name, double t0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%s %.0f ms", phases.empty() ? "" : ", ", name, msSince(t0));
    phases += buf;
  };

  // 1-2. Environment (ODBC 3.80), login timeout, SQLDriverConnectW(SQL_DRIVER_NOPROMPT).
  double t0 = nowSeconds();
  Env env;  // SQLAllocHandle(ENV) + SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION, SQL_OV_ODBC3_80)
  SQLINTEGER odbcVersion = 0;
  ODBC_CHECK(Odbc.SQLGetEnvAttr(env.h, SQL_ATTR_ODBC_VERSION, &odbcVersion, 0, nullptr), SQL_HANDLE_ENV, env.h);
  CHECK_EQ(odbcVersion, static_cast<SQLINTEGER>(SQL_OV_ODBC3_80));
  Dbc dbc(env);
  ODBC_REQUIRE(Odbc.SQLSetConnectAttrW(dbc.h, SQL_ATTR_LOGIN_TIMEOUT, attrVal(kLoginTimeoutSeconds), SQL_IS_UINTEGER),
               SQL_HANDLE_DBC, dbc.h);
  std::string outConnStr;
  const SQLRETURN connectRc = dbc.connect(ctx().connStr, &outConnStr);
  if (!SQL_SUCCEEDED(connectRc)) {
    testfw::fail(__FILE__, __LINE__,
                 std::string("SQLDriverConnectW returned ") + rcName(connectRc) + ": " + diagText(SQL_HANDLE_DBC, dbc.h), true);
  }
  CHECK(!outConnStr.empty());
  CHECK_EQ(getConnectAttrInt(dbc.h, SQL_ATTR_LOGIN_TIMEOUT), static_cast<SQLULEN>(kLoginTimeoutSeconds));
  phaseDone("connect", t0);

  // 3. SQLGetInfoW probes; the DBMS version tells the mock from a real Trino.
  t0 = nowSeconds();
  const bool mock = serverIsMock(dbc.h);
  const Target target = mock ? mockTarget() : trinoTarget();
  const int infoProbes = probeGetInfo(dbc.h, mock);
  CHECK(infoProbes >= 30);
  phaseDone("getinfo", t0);

  // 4-5. SQLGetFunctions and the type map.
  t0 = nowSeconds();
  checkGetFunctions(dbc.h);
  const size_t typeRows = readTypeInfo(dbc.h);
  phaseDone("functions+typeinfo", t0);

  // 6. Navigator.
  t0 = nowSeconds();
  Rows columnRows;
  if (!browseNavigator(dbc.h, target, columnRows)) return;
  phaseDone("navigator", t0);

  FetchStats preview;
  FetchStats import;
  {
    Stmt stmt(dbc);
    RowsetBuffers buffers(kRowset);

    // 7. Preview: prepare, describe before execute, execute, bind, block fetch.
    t0 = nowSeconds();
    const std::string previewSql =
        selectSql(previewColumnNames(target), target.catalog, target.schema, target.table, kPreviewLimit);
    ODBC_REQUIRE(prepare(stmt.h, previewSql), SQL_HANDLE_STMT, stmt.h);
    checkPreparedMetadata(stmt.h, target, columnRows);
    ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
    REQUIRE_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(target.preview.size()));
    for (size_t i = 0; i < target.preview.size(); ++i) {  // execution does not change the description
      const ColumnDesc d = describeCol(stmt.h, static_cast<SQLUSMALLINT>(i + 1));
      CHECK_EQ(d.name, std::string(target.preview[i].name));
      CHECK(sqlTypeMatches(target, d.type, target.preview[i].sqlType));
      CHECK_EQ(d.size, target.preview[i].size);
    }
    SQLLEN rowCount = 0;
    ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rowCount), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));
    bindPreviewColumns(stmt.h, buffers);
    setBlockCursor(stmt.h, kRowset, &buffers.fetched, buffers.status.data());
    preview = fetchRowsets(stmt.h, target, buffers, true);
    CHECK_EQ(preview.rows, static_cast<int64_t>(kPreviewLimit));
    CHECK_EQ(preview.rowsets, static_cast<int>((kPreviewLimit + kRowset - 1) / kRowset));
    if (target.mock) CHECK_EQ(preview.keySum, static_cast<int64_t>(kPreviewLimit) * (kPreviewLimit + 1) / 2);
    CHECK(Odbc.SQLMoreResults(stmt.h) == SQL_NO_DATA);
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    phaseDone("preview", t0);

    // 8. Import: the unlimited query on the same statement. SQL_CLOSE kept the bindings and the
    // block-cursor attributes, which is what the connector relies on when it re-runs a query.
    t0 = nowSeconds();
    const std::string importSql =
        selectSql(previewColumnNames(target), target.bigCatalog, target.bigSchema, target.bigTable, -1);
    CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), kRowset);
    const double importStart = nowSeconds();
    ODBC_REQUIRE(execDirect(stmt.h, importSql), SQL_HANDLE_STMT, stmt.h);
    import = fetchRowsets(stmt.h, target, buffers, false);
    import.seconds = nowSeconds() - importStart;  // include SQLExecDirect: the user waits for it too
    CHECK_EQ(import.rows, target.bigRows);
    CHECK_EQ(import.rowsets, static_cast<int>((target.bigRows + static_cast<int64_t>(kRowset) - 1) /
                                              static_cast<int64_t>(kRowset)));
    if (target.mock) CHECK_EQ(import.keySum, target.bigRows * (target.bigRows + 1) / 2);
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
    // Detach the status/fetched pointers before the buffers go out of scope.
    ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, nullptr, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h);
    phaseDone("import", t0);
  }  // SQLFreeHandle(SQL_HANDLE_STMT)

  // 9. Long text columns through chunked SQLGetData.
  t0 = nowSeconds();
  int getDataCalls = 0;
  const int64_t longRows = readLongText(dbc.h, target, getDataCalls);
  phaseDone("long-text", t0);

  // 10. SQLDisconnect (every statement is already freed, as the connector does).
  t0 = nowSeconds();
  ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
  CHECK(!dbc.connected());
  phaseDone("disconnect", t0);

  const double rate = import.seconds > 0 ? static_cast<double>(import.rows) / import.seconds : 0.0;
  info("Power BI import %s.%s.%s: %lld rows x %llu columns in %.2f s = %.0f rows/s (rowset %llu, %d SQLFetchScroll rowsets)",
       target.bigCatalog.c_str(), target.bigSchema.c_str(), target.bigTable.c_str(), static_cast<long long>(import.rows),
       static_cast<unsigned long long>(target.preview.size()), import.seconds, rate, static_cast<unsigned long long>(kRowset), import.rowsets);
  info("flow: %s server, %d SQLGetInfoW probes, %llu type-info rows, preview %lld rows, long-text %lld rows via %d "
       "SQLGetData chunks; %s; total %.2f s",
       target.mock ? "mock" : "Trino", infoProbes, static_cast<unsigned long long>(typeRows), static_cast<long long>(preview.rows),
       static_cast<long long>(longRows), getDataCalls, phases.c_str(), nowSeconds() - flowStart);
}
