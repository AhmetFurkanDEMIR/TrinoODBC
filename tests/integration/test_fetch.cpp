// Fetch tests: block cursors over big_table (column-wise and row-wise binding, bind offsets,
// status arrays, rows-fetched pointer, id checksums), chunked SQLGetData, SQL_ATTR_MAX_ROWS,
// closing a cursor mid-result, SQLCancel from another thread, 503 retries, server errors during
// the fetch, the spooled protocol (segment acks) and the ODBC 2 / 3 fetch entry points.
#include "integration/odbc_loader.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace harness;

namespace {

constexpr SQLULEN kRowset = 1000;
constexpr int64_t kBigRows = 250000;

std::string bigTableSql(int64_t rows) { return "SELECT * FROM big_table /* rows=" + std::to_string(rows) + " */"; }

/// A query with `rows` rows on either server (customer has 1500 rows on tpch.tiny).
std::string smallTableSql(int64_t rows) {
  if (ctx().mock) return bigTableSql(rows);
  return "SELECT custkey AS id, name, acctbal AS value FROM tpch.tiny.customer LIMIT " + std::to_string(rows);
}

/// Expected big_table cells of 0-based row `i` (mirrors big_table_row() of the mock).
std::string expectedName(int64_t i) { return "name_" + std::to_string(i + 1); }
double expectedValue(int64_t i) { return static_cast<double>(i) * 0.5; }
std::string expectedAmount(int64_t i) {
  const long long a = static_cast<long long>(i) * 125;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld.%04lld", a / 100, (a % 100) * 100);
  return buf;
}
void expectedTimestamp(int64_t i, SQL_TIMESTAMP_STRUCT& ts) {
  std::memset(&ts, 0, sizeof ts);
  ts.year = 2024;
  ts.month = 1;
  ts.day = static_cast<SQLUSMALLINT>(1 + i / 86400);  // < 31 days for every row count used here
  ts.hour = static_cast<SQLUSMALLINT>((i % 86400) / 3600);
  ts.minute = static_cast<SQLUSMALLINT>((i % 3600) / 60);
  ts.second = static_cast<SQLUSMALLINT>(i % 60);
  ts.fraction = 0;
}

bool sameTimestamp(const SQL_TIMESTAMP_STRUCT& a, const SQL_TIMESTAMP_STRUCT& b) {
  return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute &&
         a.second == b.second && a.fraction == b.fraction;
}

/// Row-wise binding record (SQL_ATTR_ROW_BIND_TYPE = sizeof(RowRec)).
struct RowRec {
  SQLBIGINT id;
  SQLLEN idInd;
  SQLDOUBLE value;
  SQLLEN valueInd;
  char name[24];
  SQLLEN nameInd;
};

/// Polls the mock statistics until `pred` holds or the timeout passes; returns the last snapshot.
template <typename Pred>
std::optional<MockStats> waitForStats(Pred pred, int timeoutMs) {
  const double deadline = nowSeconds() + timeoutMs / 1000.0;
  std::optional<MockStats> last;
  for (;;) {
    last = mockStats();
    if (last && pred(*last)) return last;
    if (nowSeconds() >= deadline) return last;
    sleepMs(50);
  }
}

}  // namespace

TEST_CASE("fetch/big_table_column_wise_250k") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  std::vector<SQLBIGINT> ids(kRowset);
  std::vector<SQLLEN> idInd(kRowset);
  std::vector<char> names(kRowset * 32);
  std::vector<SQLLEN> nameInd(kRowset);
  std::vector<SQLDOUBLE> values(kRowset);
  std::vector<SQLLEN> valueInd(kRowset);
  std::vector<SQL_TIMESTAMP_STRUCT> tss(kRowset);
  std::vector<SQLLEN> tsInd(kRowset);
  std::vector<char> amounts(kRowset * 32);
  std::vector<SQLLEN> amountInd(kRowset);
  std::vector<SQLUSMALLINT> status(kRowset, 0xFFFF);
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(kRowset), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, status.data(), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, ids.data(), sizeof(SQLBIGINT), idInd.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 2, SQL_C_CHAR, names.data(), 32, nameInd.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 3, SQL_C_DOUBLE, values.data(), sizeof(SQLDOUBLE), valueInd.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 4, SQL_C_TYPE_TIMESTAMP, tss.data(), sizeof(SQL_TIMESTAMP_STRUCT), tsInd.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 5, SQL_C_CHAR, amounts.data(), 32, amountInd.data()), SQL_HANDLE_STMT, stmt.h);

  const double t0 = nowSeconds();
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(kBigRows)), SQL_HANDLE_STMT, stmt.h);
  int64_t rows = 0;
  int64_t checksum = 0;
  int rowsets = 0;
  int badRows = 0;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetch(stmt.h);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++rowsets;
    REQUIRE(fetched >= 1 && fetched <= kRowset);
    for (SQLULEN i = 0; i < fetched; ++i) {
      const int64_t row0 = rows + static_cast<int64_t>(i);
      checksum += ids[i];
      bool ok = ids[i] == row0 + 1 && idInd[i] == static_cast<SQLLEN>(sizeof(SQLBIGINT)) && status[i] == SQL_ROW_SUCCESS &&
                valueInd[i] == static_cast<SQLLEN>(sizeof(SQLDOUBLE)) && tsInd[i] == static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT));
      // Full value check on a sparse sample keeps the loop cheap while still covering every page.
      if (ok && (row0 % 997 == 0 || i == fetched - 1)) {
        SQL_TIMESTAMP_STRUCT ts;
        expectedTimestamp(row0, ts);
        ok = std::string(&names[i * 32]) == expectedName(row0) && nameInd[i] == static_cast<SQLLEN>(expectedName(row0).size()) &&
             values[i] == expectedValue(row0) && sameTimestamp(tss[i], ts) &&
             std::string(&amounts[i * 32]) == expectedAmount(row0);
      }
      if (!ok && badRows++ < 5) {
        testfw::fail(__FILE__, __LINE__,
                     "row " + std::to_string(row0 + 1) + " mismatch: id=" + std::to_string(static_cast<long long>(ids[i])) +
                         " name=" + std::string(&names[i * 32]) + " status=" + std::to_string(status[i]),
                     false);
      }
    }
    rows += static_cast<int64_t>(fetched);
  }
  const double seconds = nowSeconds() - t0;
  CHECK_EQ(fetched, static_cast<SQLULEN>(0));  // SQL_NO_DATA sets the rows-fetched output to 0
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
  CHECK_EQ(rows, kBigRows);
  CHECK_EQ(rowsets, static_cast<int>(kBigRows / static_cast<int64_t>(kRowset)));
  CHECK_EQ(checksum, kBigRows * (kBigRows + 1) / 2);
  CHECK_EQ(badRows, 0);
  info("column-wise: %lld rows in %.2f s (%.0f rows/s), rowset %llu", static_cast<long long>(rows), seconds,
       seconds > 0 ? rows / seconds : 0.0, static_cast<unsigned long long>(kRowset));
  SQLLEN rowCount = 0;
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rowCount), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("fetch/big_table_row_wise_with_bind_offset") {
  HARNESS_MOCK_ONLY();
  const int64_t total = 5000;
  Session s;
  Stmt stmt(s.dbc);
  std::vector<RowRec> bufA(kRowset);
  std::vector<RowRec> bufB(kRowset);
  std::memset(bufA.data(), 0, bufA.size() * sizeof(RowRec));
  std::memset(bufB.data(), 0, bufB.size() * sizeof(RowRec));
  SQLLEN offset = 0;
  std::vector<SQLUSMALLINT> status(kRowset, 0xFFFF);
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_TYPE, attrVal(sizeof(RowRec)), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_BIND_TYPE), static_cast<SQLULEN>(sizeof(RowRec)));
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_OFFSET_PTR, &offset, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(kRowset), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, status.data(), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  // Bind to the first record of buffer A; the offset pointer redirects whole rowsets to buffer B.
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, &bufA[0].id, sizeof(SQLBIGINT), &bufA[0].idInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 3, SQL_C_DOUBLE, &bufA[0].value, sizeof(SQLDOUBLE), &bufA[0].valueInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 2, SQL_C_CHAR, bufA[0].name, sizeof(bufA[0].name), &bufA[0].nameInd), SQL_HANDLE_STMT, stmt.h);

  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(total)), SQL_HANDLE_STMT, stmt.h);
  int64_t rows = 0;
  int64_t checksum = 0;
  int rowsets = 0;
  for (;;) {
    const bool useB = rowsets % 2 == 1;
    offset = useB ? static_cast<SQLLEN>(reinterpret_cast<char*>(bufB.data()) - reinterpret_cast<char*>(bufA.data())) : 0;
    std::vector<RowRec>& target = useB ? bufB : bufA;
    std::vector<RowRec>& other = useB ? bufA : bufB;
    const SQLBIGINT otherFirstId = other[0].id;
    const SQLRETURN rc = Odbc.SQLFetch(stmt.h);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++rowsets;
    CHECK_EQ(other[0].id, otherFirstId);  // the other buffer was not written
    int bad = 0;
    for (SQLULEN i = 0; i < fetched; ++i) {
      const int64_t row0 = rows + static_cast<int64_t>(i);
      const RowRec& r = target[i];
      checksum += r.id;
      const bool ok = r.id == row0 + 1 && r.idInd == static_cast<SQLLEN>(sizeof(SQLBIGINT)) && r.value == expectedValue(row0) &&
                      std::string(r.name) == expectedName(row0) && r.nameInd == static_cast<SQLLEN>(expectedName(row0).size()) &&
                      status[i] == SQL_ROW_SUCCESS;
      if (!ok) ++bad;
    }
    CHECK_EQ(bad, 0);
    rows += static_cast<int64_t>(fetched);
  }
  CHECK_EQ(rows, total);
  CHECK_EQ(rowsets, 5);
  CHECK_EQ(checksum, total * (total + 1) / 2);
  // Rowsets 1, 3, 5 landed in A and 2, 4 in B: the last writes are visible, earlier ones overwritten.
  CHECK_EQ(bufA[0].id, static_cast<SQLBIGINT>(4001));
  CHECK_EQ(bufA[999].id, static_cast<SQLBIGINT>(5000));
  CHECK_EQ(bufB[0].id, static_cast<SQLBIGINT>(3001));
  CHECK_EQ(bufB[999].id, static_cast<SQLBIGINT>(4000));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_OFFSET_PTR, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_TYPE, attrVal(SQL_BIND_BY_COLUMN), 0), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("fetch/short_last_rowset_status_array") {
  Session s;
  Stmt stmt(s.dbc);
  const int64_t total = 1250;
  const SQLULEN rowset = 500;
  std::vector<SQLBIGINT> ids(rowset);
  std::vector<SQLLEN> idInd(rowset);
  std::vector<SQLUSMALLINT> status(rowset, 0xFFFF);
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(rowset), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, status.data(), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, ids.data(), sizeof(SQLBIGINT), idInd.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(total)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, rowset);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(1));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, rowset);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(501));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(250));
  CHECK_EQ(status[249], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS));
  CHECK_EQ(status[250], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
  CHECK_EQ(status[499], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
  CHECK_EQ(fetched, static_cast<SQLULEN>(0));
  // Changing the rowset size while the cursor is open is allowed (it is not a cursor attribute).
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("fetch/getdata_chunked_char_7_bytes") {
  Session s("AnsiEncoding=utf8");
  Stmt stmt(s.dbc);
  const std::string text = "abcdefghijklmnopqrstuvwxyz0123456789!";  // 37 bytes: six full chunks + one
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + text + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  std::string assembled;
  int calls = 0;
  for (;;) {
    char buf[7];
    std::memset(buf, 'X', sizeof buf);
    SQLLEN ind = 0;
    const SQLRETURN rc = Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, buf, sizeof buf, &ind);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++calls;
    // The indicator reports the bytes still available before this call.
    CHECK_EQ(ind, static_cast<SQLLEN>(text.size() - assembled.size()));
    const size_t remaining = text.size() - assembled.size();
    if (remaining > 6) {
      CHECK(rc == SQL_SUCCESS_WITH_INFO);
      ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01004");
      CHECK(buf[6] == 0);
      assembled.append(buf, 6);
    } else {
      CHECK(rc == SQL_SUCCESS);
      CHECK(buf[remaining] == 0);
      assembled.append(buf, remaining);
    }
    REQUIRE(calls < 100);
  }
  CHECK_EQ(calls, 7);
  CHECK_EQ(assembled, text);
  // Once SQL_NO_DATA was returned the column stays exhausted; other columns are unaffected.
  char again[8];
  SQLLEN ind = 0;
  CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, again, sizeof again, &ind) == SQL_NO_DATA);
  // A zero-length buffer only reports the length; a null buffer likewise.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + text + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, nullptr, 0, &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(ind, static_cast<SQLLEN>(text.size()));
  CHECK_EQ(getString(stmt.h, 1, nullptr, 5), text);  // still fully readable afterwards
}

TEST_CASE("fetch/getdata_chunked_wchar_3_units") {
  Session s;
  Stmt stmt(s.dbc);
  const std::string text = "abcdefghijklmnopqrstuvwxyz0123456789!";  // 37 chars: 18 chunks of 2 + one
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + text + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const SQLLEN unit = static_cast<SQLLEN>(sizeof(SQLWCHAR));
  std::string assembled;
  int calls = 0;
  for (;;) {
    SQLWCHAR buf[3] = {0x7777, 0x7777, 0x7777};
    SQLLEN ind = 0;
    const SQLRETURN rc = Odbc.SQLGetData(stmt.h, 1, SQL_C_WCHAR, buf, 3 * unit, &ind);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++calls;
    const size_t remaining = text.size() - assembled.size();
    CHECK_EQ(ind, static_cast<SQLLEN>(remaining) * unit);  // bytes still available
    if (remaining > 2) {
      CHECK(rc == SQL_SUCCESS_WITH_INFO);
      ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01004");
      CHECK(buf[2] == 0);
      assembled += fromWide(buf, 2);
    } else {
      CHECK(rc == SQL_SUCCESS);
      CHECK(buf[remaining] == 0);
      assembled += fromWide(buf, static_cast<SQLLEN>(remaining));
    }
    REQUIRE(calls < 100);
  }
  CHECK_EQ(calls, 19);
  CHECK_EQ(assembled, text);
  // A multi-byte character never gets split across chunks (UTF-16 surrogates stay together).
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const std::string emoji = "a😀b😀c";
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + emoji + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getWString(stmt.h, 1, nullptr, 3 * unit), emoji);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + emoji + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // A 2-byte payload cannot hold a 4-byte code point: the driver splits it (SQL_C_CHAR is a byte
  // stream) instead of looping forever; the pieces still reassemble to the original text.
  CHECK_EQ(getString(stmt.h, 1, nullptr, 3), emoji);
}

TEST_CASE("fetch/max_rows_attribute") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_ROWS, attrVal(10), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(1000)), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(10));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // A rowset larger than the limit is cut at the limit.
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(64), 0), SQL_HANDLE_STMT, stmt.h);
  SQLULEN fetched = 0;
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(1000)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(10));
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
  // Back to unlimited.
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_ROWS, attrVal(0), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(25)), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(25));
}

TEST_CASE("fetch/close_cursor_mid_result_then_rerun") {
  Session s;
  Stmt stmt(s.dbc);
  const int64_t total = ctx().mock ? 20000 : 1500;
  const std::optional<MockStats> before = mockStats();
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(total)), SQL_HANDLE_STMT, stmt.h);
  for (int i = 0; i < 3; ++i) ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(3));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");  // already closed
  if (ctx().mock && before) {
    // The abandoned query is cancelled server-side (DELETE nextUri, sent asynchronously).
    const std::optional<MockStats> after = waitForStats([&](const MockStats& m) { return m.cancelled > before->cancelled; }, 3000);
    REQUIRE(after.has_value());
    CHECK(after->cancelled > before->cancelled);
  }
  // The same statement runs the query again from the start.
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(total)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  CHECK_EQ(countRows(stmt.h) + 1, total);
  // SQLFreeStmt(SQL_CLOSE) is the lenient form: fine with or without a cursor.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  // Freeing a statement with an open cursor is allowed and cancels the query as well.
  {
    Stmt gone(s.dbc);
    ODBC_REQUIRE(execDirect(gone.h, smallTableSql(total)), SQL_HANDLE_STMT, gone.h);
    ODBC_REQUIRE(Odbc.SQLFetch(gone.h), SQL_HANDLE_STMT, gone.h);
  }
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("fetch/cancel_from_other_thread_hy008") {
  HARNESS_MOCK_ONLY();
  Session s;
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  {
    Stmt stmt(s.dbc);
    std::atomic<int> cancelRc{-999};
    std::thread canceller([&] {
      sleepMs(400);
      cancelRc.store(static_cast<int>(Odbc.SQLCancel(stmt.h)));
    });
    const double t0 = nowSeconds();
    const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM slow_table");
    const double elapsed = nowSeconds() - t0;
    canceller.join();
    ODBC_CHECK_ERROR(rc, SQL_HANDLE_STMT, stmt.h, "HY008");
    CHECK_EQ(cancelRc.load(), static_cast<int>(SQL_SUCCESS));
    CHECK(elapsed < 4.5);  // slow_table needs >= 5 s when not cancelled
    info("cancelled after %.2f s", elapsed);
    // The statement is usable again.
    ODBC_CHECK(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  }
  const std::optional<MockStats> after = waitForStats([&](const MockStats& m) { return m.cancelled > before->cancelled; }, 3000);
  REQUIRE(after.has_value());
  CHECK(after->cancelled > before->cancelled);
  // SQLCancelHandle on the connection cancels the statement's query the same way.
  {
    Stmt stmt(s.dbc);
    std::thread canceller([&] {
      sleepMs(400);
      Odbc.SQLCancelHandle(SQL_HANDLE_DBC, s.h());
    });
    const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM slow_table");
    canceller.join();
    ODBC_CHECK_ERROR(rc, SQL_HANDLE_STMT, stmt.h, "HY008");
  }
  // SQLCancel on an idle statement is a no-op success.
  Stmt idle(s.dbc);
  ODBC_CHECK(Odbc.SQLCancel(idle.h), SQL_HANDLE_STMT, idle.h);
  ODBC_CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_STMT, idle.h), SQL_HANDLE_STMT, idle.h);
  CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_ENV, s.env.h) == SQL_INVALID_HANDLE);
}

TEST_CASE("fetch/flaky_table_retries_503") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  const double t0 = nowSeconds();
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM flaky_table"), SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(2));
  CHECK_EQ(rows[0][0], std::string("1"));
  CHECK_EQ(rows[0][1], std::string("flaky"));
  CHECK_EQ(rows[1][1], std::string("but fine"));
  info("three 503 answers survived in %.2f s", nowSeconds() - t0);
  // With retries disabled the same query fails with a transport error.
  Session none("MaxRetries=0");
  Stmt fail(none.dbc);
  const SQLRETURN rc = execDirect(fail.h, "SELECT * FROM flaky_table");
  CHECK(rc == SQL_ERROR);
  CHECK(hasState(SQL_HANDLE_STMT, fail.h, "08S01"));
}

TEST_CASE("fetch/error_table_22012_during_fetch") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  // The first page arrives fine; the failure comes with the second response.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM error_table"), SQL_HANDLE_STMT, stmt.h);
  int64_t rows = 0;
  SQLRETURN rc = SQL_SUCCESS;
  for (;;) {
    rc = Odbc.SQLFetch(stmt.h);
    if (!SQL_SUCCEEDED(rc)) break;
    ++rows;
    REQUIRE(rows <= 5000);
  }
  CHECK(rc == SQL_ERROR);
  ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "22012");
  const std::vector<DiagRec> d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!d.empty());
  CHECK_EQ(d.front().native, static_cast<SQLINTEGER>(65545));
  CHECK(d.front().message.rfind("[Trino][ODBC Driver][Trino] Division by zero", 0) == 0);
  CHECK(rows >= 1);  // the first page (1000 rows by default) was delivered before the error
  info("%lld rows delivered before the server error", static_cast<long long>(rows));
  // The failure is sticky for this cursor; closing it and re-running works.
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_ERROR);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("fetch/spooled_segments_are_acked") {
  HARNESS_MOCK_ONLY();
  const int64_t rows = 20000;  // 20 pages: one inline segment + 19 spooled ones
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  {
    Session s;
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(kRowset), 0), SQL_HANDLE_STMT, stmt.h);
    std::vector<SQLBIGINT> ids(kRowset);
    std::vector<SQLLEN> ind(kRowset);
    SQLULEN fetched = 0;
    ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, ids.data(), sizeof(SQLBIGINT), ind.data()), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(rows)), SQL_HANDLE_STMT, stmt.h);
    int64_t total = 0;
    int64_t checksum = 0;
    for (;;) {
      const SQLRETURN rc = Odbc.SQLFetch(stmt.h);
      if (rc == SQL_NO_DATA) break;
      ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
      for (SQLULEN i = 0; i < fetched; ++i) checksum += ids[i];
      total += static_cast<int64_t>(fetched);
    }
    CHECK_EQ(total, rows);
    CHECK_EQ(checksum, rows * (rows + 1) / 2);  // segments delivered in rowOffset order, none twice
  }
  const std::optional<MockStats> after =
      waitForStats([&](const MockStats& m) { return m.acks - before->acks >= 19; }, 5000);
  REQUIRE(after.has_value());
  CHECK(after->segmentsServed - before->segmentsServed >= 19);
  CHECK(after->acks - before->acks >= 19);
  info("spooled segments served: %lld, acked: %lld", static_cast<long long>(after->segmentsServed - before->segmentsServed),
       static_cast<long long>(after->acks - before->acks));
  // Spooling=0 uses the classic protocol: no segment traffic at all.
  {
    Session plain("Spooling=0");
    Stmt stmt(plain.dbc);
    ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(3000)), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(3000));
  }
  const std::optional<MockStats> classic = mockStats();
  REQUIRE(classic.has_value());
  CHECK_EQ(classic->segmentsServed, after->segmentsServed);
}

TEST_CASE("fetch/fetchscroll_and_extended_fetch") {
  Session s;
  Stmt stmt(s.dbc);
  const int64_t total = 7;
  const std::string sql = ctx().mock ? bigTableSql(total) : "SELECT custkey FROM tpch.tiny.customer LIMIT 7";
  std::vector<SQLBIGINT> ids(3);
  std::vector<SQLLEN> ind(3);
  std::vector<SQLUSMALLINT> status(3, 0xFFFF);
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(3), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, status.data(), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, ids.data(), sizeof(SQLBIGINT), ind.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, sql), SQL_HANDLE_STMT, stmt.h);
  // Only SQL_FETCH_NEXT exists on a forward-only cursor.
  ODBC_CHECK_ERROR(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_PRIOR, 0), SQL_HANDLE_STMT, stmt.h, "HY106");
  ODBC_CHECK_ERROR(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_ABSOLUTE, 2), SQL_HANDLE_STMT, stmt.h, "HY106");
  ODBC_REQUIRE(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(3));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(1));
  ODBC_REQUIRE(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(3));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(4));
  ODBC_REQUIRE(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(1));
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS));
  CHECK_EQ(status[1], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
  CHECK_EQ(status[2], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
  if (ctx().mock) CHECK_EQ(ids[0], static_cast<SQLBIGINT>(7));
  CHECK(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_NEXT, 0) == SQL_NO_DATA);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // ODBC 2 block fetch: rowset size through SQL_ROWSET_SIZE, outputs through the call's arguments.
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtOption(stmt.h, SQL_ROWSET_SIZE, 3), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, sql), SQL_HANDLE_STMT, stmt.h);
  SQLULEN crow = 0;
  SQLUSMALLINT rowStatus[3] = {0xFFFF, 0xFFFF, 0xFFFF};
  ODBC_CHECK_ERROR(Odbc.SQLExtendedFetch(stmt.h, SQL_FETCH_FIRST, 0, &crow, rowStatus), SQL_HANDLE_STMT, stmt.h, "HY106");
  int64_t rows = 0;
  int calls = 0;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLExtendedFetch(stmt.h, SQL_FETCH_NEXT, 0, &crow, rowStatus);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    ++calls;
    for (SQLULEN i = 0; i < crow; ++i) {
      CHECK_EQ(rowStatus[i], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS));
      if (ctx().mock) CHECK_EQ(ids[i], static_cast<SQLBIGINT>(rows + static_cast<int64_t>(i) + 1));
    }
    for (SQLULEN i = crow; i < 3; ++i) CHECK_EQ(rowStatus[i], static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
    rows += static_cast<int64_t>(crow);
    REQUIRE(calls < 10);
  }
  CHECK_EQ(rows, total);
  CHECK_EQ(calls, 3);
  CHECK_EQ(crow, static_cast<SQLULEN>(0));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("fetch/setpos_position_within_block") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(4), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(4)), SQL_HANDLE_STMT, stmt.h);
  // SQLSetPos needs a fetched rowset.
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Without SQLSetPos, SQLGetData reads the first row of the rowset (SQL_GD_BLOCK).
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLSetPos(stmt.h, 3, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(3));
  CHECK_EQ(getString(stmt.h, 2), std::string("name_3"));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(3));
  // Re-positioning resets the SQLGetData bookkeeping, so a column can be read again.
  ODBC_CHECK(Odbc.SQLSetPos(stmt.h, 4, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(4));
  ODBC_CHECK(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  // Row 0 means "all rows" which SQL_POSITION cannot take; beyond the rowset is out of range.
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 0, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "HY109");
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 5, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "HY107");
  // Read-only cursor: no updates, no locks.
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_REFRESH, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_EXCLUSIVE), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLBulkOperations(stmt.h, SQL_ADD), SQL_HANDLE_STMT, stmt.h, "HYC00");
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
}

TEST_CASE("fetch/big_result_real_server") {
  if (ctx().mock) {
    skip("real-server variant of the big fetch (tpch.sf1.orders)");
    return;
  }
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(kRowset), 0), SQL_HANDLE_STMT, stmt.h);
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  const double t0 = nowSeconds();
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM tpch.sf1.orders"), SQL_HANDLE_STMT, stmt.h);
  const SQLSMALLINT n = numResultCols(stmt.h);
  // Generic binding by described type; text columns get 128 wide characters each.
  std::vector<std::vector<unsigned char>> buffers(static_cast<size_t>(n));
  std::vector<std::vector<SQLLEN>> inds(static_cast<size_t>(n), std::vector<SQLLEN>(kRowset));
  for (SQLSMALLINT c = 1; c <= n; ++c) {
    const ColumnDesc d = describeCol(stmt.h, static_cast<SQLUSMALLINT>(c));
    SQLSMALLINT cType = SQL_C_WCHAR;
    size_t elem = 128 * sizeof(SQLWCHAR);
    if (d.type == SQL_BIGINT || d.type == SQL_INTEGER || d.type == SQL_SMALLINT || d.type == SQL_TINYINT) {
      cType = SQL_C_SBIGINT;
      elem = sizeof(SQLBIGINT);
    } else if (d.type == SQL_DOUBLE || d.type == SQL_REAL || d.type == SQL_FLOAT) {
      cType = SQL_C_DOUBLE;
      elem = sizeof(SQLDOUBLE);
    } else if (d.type == SQL_TYPE_DATE) {
      cType = SQL_C_TYPE_DATE;
      elem = sizeof(SQL_DATE_STRUCT);
    } else if (d.type == SQL_TYPE_TIMESTAMP) {
      cType = SQL_C_TYPE_TIMESTAMP;
      elem = sizeof(SQL_TIMESTAMP_STRUCT);
    }
    std::vector<unsigned char>& buf = buffers[static_cast<size_t>(c - 1)];
    buf.resize(elem * kRowset);
    ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, static_cast<SQLUSMALLINT>(c), cType, buf.data(), static_cast<SQLLEN>(elem),
                                 inds[static_cast<size_t>(c - 1)].data()),
                 SQL_HANDLE_STMT, stmt.h);
  }
  int64_t rows = 0;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetch(stmt.h);
    if (rc == SQL_NO_DATA) break;
    ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
    rows += static_cast<int64_t>(fetched);
  }
  const double seconds = nowSeconds() - t0;
  CHECK_EQ(rows, static_cast<int64_t>(1500000));
  info("tpch.sf1.orders: %lld rows in %.2f s (%.0f rows/s)", static_cast<long long>(rows), seconds,
       seconds > 0 ? rows / seconds : 0.0);
}

// ---- cancel semantics, statement release, ODBC 2 rowsets, SQL_ATTR_MAX_LENGTH ------------------

TEST_CASE("fetch/cancel_idle_open_cursor_has_no_effect") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(25)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  // With no function in progress and a result the server has finished delivering (25 rows fit in
  // one page), SQLCancel has no effect (ODBC 3.x); the cursor stays usable. A result that is still
  // streaming is cancelled instead (cancel/sqlcancel_between_fetches_stops_server_query).
  ODBC_CHECK(Odbc.SQLCancel(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_STMT, stmt.h), SQL_HANDLE_STMT, stmt.h);
  // SQLCancelHandle on the connection does not touch such a finished result either.
  ODBC_CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_DBC, s.h()), SQL_HANDLE_DBC, s.h());
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(2));
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(23));
}

TEST_CASE("fetch/cancel_handle_dbc_leaves_no_stale_cancel") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_DBC, s.h()), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLCancel(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // The lazy DESCRIBE OUTPUT of the prepared statement must not see a cancel request.
  SQLSMALLINT cols = 0;
  ODBC_CHECK(Odbc.SQLNumResultCols(stmt.h, &cols), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(cols, static_cast<SQLSMALLINT>(1));
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));

  // A cancel that aborted one call does not leak into the next one on the statement.
  std::thread canceller([&] {
    sleepMs(400);
    Odbc.SQLCancel(stmt.h);
  });
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM slow_table");
  canceller.join();
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_STMT, stmt.h, "HY008");
  ODBC_REQUIRE(prepare(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLNumResultCols(stmt.h, &cols), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("fetch/cancel_handle_dbc_concurrent_with_connect_disconnect") {
  HARNESS_MOCK_ONLY();
  // SQLCancelHandle(DBC) must never touch the connection's client while another thread connects or
  // disconnects it (under ASan the old code reported a heap-use-after-free here).
  Env env;
  Dbc dbc(env);
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread canceller([&] {
    while (!stop.load()) {
      if (Odbc.SQLCancelHandle(SQL_HANDLE_DBC, dbc.h) != SQL_SUCCESS) bad.fetch_add(1);
    }
  });
  for (int i = 0; i < 15; ++i) {
    ODBC_CHECK(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
    ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
  }
  stop.store(true);
  canceller.join();
  CHECK_EQ(bad.load(), 0);
}

TEST_CASE("fetch/free_stmt_drop_cancels_blocked_execute") {
  HARNESS_MOCK_ONLY();
  Session s;
  SQLHSTMT h = SQL_NULL_HSTMT;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, s.h(), &h), SQL_HANDLE_DBC, s.h());
  std::atomic<int> freeRc{-999};
  std::atomic<double> freeSeconds{0};
  std::thread freer([&] {
    sleepMs(400);
    const double t0 = nowSeconds();
    // SQL_DROP cancels the execution in progress instead of waiting behind it.
    freeRc.store(static_cast<int>(Odbc.SQLFreeStmt(h, SQL_DROP)));
    freeSeconds.store(nowSeconds() - t0);
  });
  const double t0 = nowSeconds();
  const SQLRETURN rc = execDirect(h, "SELECT * FROM slow_table");
  const double elapsed = nowSeconds() - t0;
  freer.join();
  CHECK(rc == SQL_ERROR);
  CHECK(elapsed < 4.5);  // slow_table needs >= 5 s when not cancelled
  CHECK_EQ(freeRc.load(), static_cast<int>(SQL_SUCCESS));
  CHECK(freeSeconds.load() < 4.5);
  CHECK(Odbc.SQLFreeStmt(h, SQL_CLOSE) == SQL_INVALID_HANDLE);
  // The connection is intact.
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("fetch/setscrolloptions_sets_odbc2_rowset_only") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  std::vector<SQLBIGINT> ids(4, 0);
  std::vector<SQLLEN> ind(4, 0);
  ODBC_REQUIRE(Odbc.SQLSetScrollOptions(stmt.h, SQL_CONCUR_READ_ONLY, SQL_SCROLL_FORWARD_ONLY, 4), SQL_HANDLE_STMT, stmt.h);
  // SQL_ROWSET_SIZE (SQLExtendedFetch) and SQL_ATTR_ROW_ARRAY_SIZE (SQLFetch) are separate.
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), static_cast<SQLULEN>(1));
  SQLULEN rowset = 0;
  ODBC_CHECK(Odbc.SQLGetStmtOption(stmt.h, SQL_ROWSET_SIZE, &rowset), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowset, static_cast<SQLULEN>(4));
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, ids.data(), sizeof(SQLBIGINT), ind.data()), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(10)), SQL_HANDLE_STMT, stmt.h);
  SQLULEN crow = 0;
  SQLUSMALLINT status[4] = {0, 0, 0, 0};
  ODBC_REQUIRE(Odbc.SQLExtendedFetch(stmt.h, SQL_FETCH_NEXT, 0, &crow, status), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(crow, static_cast<SQLULEN>(4));
  CHECK_EQ(ids[3], static_cast<SQLBIGINT>(4));
  // SQLFetch keeps its own rowset of one row.
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(ids[0], static_cast<SQLBIGINT>(5));
  CHECK_EQ(ids[1], static_cast<SQLBIGINT>(2));  // untouched by the one-row rowset
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("fetch/row_number_after_setpos_in_later_rowset") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(10), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(25)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(11));
  ODBC_REQUIRE(Odbc.SQLSetPos(stmt.h, 3, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(13));
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(13));
  ODBC_REQUIRE(Odbc.SQLSetPos(stmt.h, 10, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_NUMBER), static_cast<SQLULEN>(20));
}

TEST_CASE("fetch/max_length_caps_character_data") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  const std::string value(100, 'x');
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_LENGTH, attrVal(10), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + value + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Data beyond SQL_ATTR_MAX_LENGTH does not exist for the application: SQL_SUCCESS (no 01004),
  // the capped length, and SQL_NO_DATA afterwards.
  char buf[1000];
  SQLLEN ind = 0;
  CHECK_EQ(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, buf, sizeof buf, &ind), static_cast<SQLRETURN>(SQL_SUCCESS));
  CHECK_EQ(ind, static_cast<SQLLEN>(10));
  CHECK_EQ(std::string(buf), std::string(10, 'x'));
  CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, buf, sizeof buf, &ind) == SQL_NO_DATA);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // Chunked retrieval inside the capped data: 01004 while pieces remain, 10 bytes in total.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + value + "'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  std::string got;
  std::vector<SQLLEN> lengths;
  for (int i = 0; i < 20; ++i) {
    char piece[4];
    const SQLRETURN rc = Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, piece, sizeof piece, &ind);
    if (rc == SQL_NO_DATA) break;
    REQUIRE(SQL_SUCCEEDED(rc));
    lengths.push_back(ind);
    got += piece;
    if (rc == SQL_SUCCESS) {
      CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, piece, sizeof piece, &ind) == SQL_NO_DATA);
      break;
    }
    ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01004");
  }
  CHECK_EQ(got, std::string(10, 'x'));
  REQUIRE_EQ(lengths.size(), static_cast<size_t>(4));
  CHECK_EQ(lengths[0], static_cast<SQLLEN>(10));
  CHECK_EQ(lengths[3], static_cast<SQLLEN>(1));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // Bound columns are capped the same way.
  char bound[64];
  SQLLEN boundInd = 0;
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_CHAR, bound, sizeof bound, &boundInd), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '" + value + "'"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(Odbc.SQLFetch(stmt.h), static_cast<SQLRETURN>(SQL_SUCCESS));
  CHECK_EQ(boundInd, static_cast<SQLLEN>(10));
  CHECK_EQ(std::string(bound), std::string(10, 'x'));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // Only character and binary columns are limited: a number rendered as text stays whole.
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_LENGTH, attrVal(3), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1234567"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, buf, sizeof buf, &ind), static_cast<SQLRETURN>(SQL_SUCCESS));
  CHECK_EQ(std::string(buf), std::string("1234567"));
}

TEST_CASE("fetch/odbc2_environment_reports_legacy_datetime_types") {
  HARNESS_MOCK_ONLY();
  // ODBC 2 applications know SQL_DATE / SQL_TIME / SQL_TIMESTAMP (9/10/11), not 91/92/93.
  Env env(SQL_OV_ODBC2);
  Dbc dbc(env);
  ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
  {
    Stmt stmt(dbc);
    ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(1)), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(describeCol(stmt.h, 4).type, static_cast<SQLSMALLINT>(SQL_TIMESTAMP));
    CHECK_EQ(colAttrNum(stmt.h, 4, SQL_COLUMN_TYPE), static_cast<SQLLEN>(SQL_TIMESTAMP));
    CHECK_EQ(colAttrNum(stmt.h, 4, SQL_DESC_CONCISE_TYPE), static_cast<SQLLEN>(SQL_TIMESTAMP));
    CHECK_EQ(describeCol(stmt.h, 1).type, static_cast<SQLSMALLINT>(SQL_BIGINT));
  }
  Session s3;  // ODBC 3.8 environment: the ODBC 3 codes
  Stmt stmt3(s3.dbc);
  ODBC_REQUIRE(execDirect(stmt3.h, bigTableSql(1)), SQL_HANDLE_STMT, stmt3.h);
  CHECK_EQ(describeCol(stmt3.h, 4).type, static_cast<SQLSMALLINT>(SQL_TYPE_TIMESTAMP));
}

TEST_CASE("fetch/getdata_ard_type_without_binding") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLHDESC ard = SQL_NULL_HDESC;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &ard, sizeof ard, nullptr), SQL_HANDLE_STMT, stmt.h);
  // Only the type, precision and scale are set: the column is not bound (no data pointer).
  ODBC_REQUIRE(Odbc.SQLSetDescField(ard, 5, SQL_DESC_TYPE, attrVal(SQL_C_NUMERIC), 0), SQL_HANDLE_DESC, ard);
  ODBC_REQUIRE(Odbc.SQLSetDescField(ard, 5, SQL_DESC_PRECISION, attrVal(18), 0), SQL_HANDLE_DESC, ard);
  ODBC_REQUIRE(Odbc.SQLSetDescField(ard, 5, SQL_DESC_SCALE, attrVal(4), 0), SQL_HANDLE_DESC, ard);
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(2)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);  // amount of row 2 = 1.2500
  SQL_NUMERIC_STRUCT num;
  std::memset(&num, 0, sizeof num);
  SQLLEN ind = 0;
  ODBC_REQUIRE(Odbc.SQLGetData(stmt.h, 5, SQL_ARD_TYPE, &num, sizeof num, &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(static_cast<int>(num.precision), 18);
  CHECK_EQ(static_cast<int>(num.scale), 4);
  CHECK_EQ(static_cast<int>(num.sign), 1);
  CHECK_EQ(static_cast<int>(num.val[0]) | (static_cast<int>(num.val[1]) << 8), 12500);
}

TEST_CASE("fetch/bind_numbers_above_32767_are_rejected") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLBIGINT id = 0;
  SQLLEN idInd = 0;
  char other[16];
  SQLLEN otherInd = 0;
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, &id, sizeof id, &idInd), SQL_HANDLE_STMT, stmt.h);
  // 40000 would wrap to a negative SQLSMALLINT and alias the binding of column 1.
  ODBC_CHECK_ERROR(Odbc.SQLBindCol(stmt.h, 40000, SQL_C_CHAR, other, sizeof other, &otherInd), SQL_HANDLE_STMT, stmt.h, "07009");
  SQLINTEGER v = 1;
  SQLLEN vInd = 0;
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 40000, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &vInd),
                   SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_REQUIRE(execDirect(stmt.h, bigTableSql(3)), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(id, static_cast<SQLBIGINT>(1));
  CHECK_EQ(idInd, static_cast<SQLLEN>(sizeof(SQLBIGINT)));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
}

// ---- server-side cancel of abandoned / cancelled results (Power BI cancel flow) ----------------
// Power BI cancels a refresh by calling SQLCancel from another thread while no ODBC call is in
// progress (between two SQLFetch calls) and then frees the handles, or simply stops calling the
// driver. Every one of those paths must stop the query on the coordinator (DELETE nextUri); the
// mock counts the DELETEs that hit a running query. Each check allows 2 s for the asynchronous
// DELETE helper.

namespace {

constexpr int kCancelWaitMs = 2000;
constexpr int64_t kLongRows = 100000;  // far more than the prefetch queue holds: the query keeps running

/// A result the server is still producing while the application reads its first rows.
std::string longResultSql(const char* tag) {
  if (ctx().mock) return "SELECT * FROM big_table /* rows=" + std::to_string(kLongRows) + " " + tag + " */";
  return std::string("SELECT * FROM tpch.sf1.lineitem /* ") + tag + " */";
}

/// Waits (2 s) for the mock's cancelled counter to pass `before`; true when it did (always true
/// on a real server, where the counter is not observable).
bool cancelReachedServer(const std::optional<MockStats>& before) {
  if (!ctx().mock || !before) return true;
  const std::optional<MockStats> after =
      waitForStats([&](const MockStats& m) { return m.cancelled > before->cancelled; }, kCancelWaitMs);
  return after && after->cancelled > before->cancelled;
}

/// Executes `sql` and fetches `rows` single-row rowsets.
void openAndRead(SQLHSTMT h, const std::string& sql, int rows) {
  ODBC_REQUIRE(execDirect(h, sql), SQL_HANDLE_STMT, h);
  for (int i = 0; i < rows; ++i) ODBC_REQUIRE(Odbc.SQLFetch(h), SQL_HANDLE_STMT, h);
}

}  // namespace

TEST_CASE("cancel/sqlcancel_between_fetches_stops_server_query") {
  Session s;
  Stmt stmt(s.dbc);
  const std::optional<MockStats> before = mockStats();
  openAndRead(stmt.h, longResultSql("cancel-between-fetches"), 3);
  if (ctx().mock) CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(3));
  // No function is in progress: SQLCancel still stops the running server query.
  const double t0 = nowSeconds();
  ODBC_CHECK(Odbc.SQLCancel(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK(nowSeconds() - t0 < 0.5);  // never waits for the DELETE
  CHECK(cancelReachedServer(before));
  // The cursor reports the cancel at the next fetch, however many rows were buffered.
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY008");
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY008");
  // A second SQLCancel is harmless; closing the cursor and re-running works.
  ODBC_CHECK(Odbc.SQLCancel(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(25)), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(25));
}

TEST_CASE("cancel/sqlcancel_from_other_thread_between_fetches") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  openAndRead(stmt.h, longResultSql("cancel-other-thread"), 2);
  std::atomic<int> rc{-999};
  std::thread canceller([&] { rc.store(static_cast<int>(Odbc.SQLCancel(stmt.h))); });
  canceller.join();
  CHECK_EQ(rc.load(), static_cast<int>(SQL_SUCCESS));
  CHECK(cancelReachedServer(before));
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY008");
}

TEST_CASE("cancel/cancel_handle_dbc_stops_idle_running_cursors") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt running(s.dbc);
  Stmt done(s.dbc);
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  openAndRead(running.h, longResultSql("cancel-handle-dbc"), 1);
  // A small result the server has finished delivering is not affected.
  openAndRead(done.h, bigTableSql(25), 1);
  ODBC_CHECK(Odbc.SQLCancelHandle(SQL_HANDLE_DBC, s.h()), SQL_HANDLE_DBC, s.h());
  CHECK(cancelReachedServer(before));
  ODBC_CHECK_ERROR(Odbc.SQLFetch(running.h), SQL_HANDLE_STMT, running.h, "HY008");
  ODBC_REQUIRE(Odbc.SQLFetch(done.h), SQL_HANDLE_STMT, done.h);
  CHECK_EQ(getInt64(done.h, 1).value_or(-1), static_cast<int64_t>(2));
  CHECK_EQ(countRows(done.h), static_cast<int64_t>(23));
}

TEST_CASE("cancel/sqlcancel_during_execute_reaches_server_quickly") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  std::thread canceller([&] {
    sleepMs(300);
    Odbc.SQLCancel(stmt.h);
  });
  const SQLRETURN rc = execDirect(stmt.h, "SELECT * FROM slow_table");
  canceller.join();
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_STMT, stmt.h, "HY008");
  CHECK(cancelReachedServer(before));
}

TEST_CASE("cancel/close_cursor_and_free_stmt_mid_result_stop_server_query") {
  HARNESS_MOCK_ONLY();
  Session s;
  // SQLCloseCursor
  {
    Stmt stmt(s.dbc);
    const std::optional<MockStats> before = mockStats();
    openAndRead(stmt.h, longResultSql("close-cursor"), 2);
    ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK(cancelReachedServer(before));
  }
  // SQLFreeStmt(SQL_CLOSE)
  {
    Stmt stmt(s.dbc);
    const std::optional<MockStats> before = mockStats();
    openAndRead(stmt.h, longResultSql("free-stmt-close"), 2);
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    CHECK(cancelReachedServer(before));
  }
  // SQLFreeHandle(SQL_HANDLE_STMT) with the cursor still open
  {
    SQLHSTMT h = SQL_NULL_HSTMT;
    ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, s.h(), &h), SQL_HANDLE_DBC, s.h());
    const std::optional<MockStats> before = mockStats();
    openAndRead(h, longResultSql("free-handle"), 2);
    CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, h) == SQL_SUCCESS);
    CHECK(cancelReachedServer(before));
  }
  // SQLMoreResults moves past the only result
  {
    Stmt stmt(s.dbc);
    const std::optional<MockStats> before = mockStats();
    openAndRead(stmt.h, longResultSql("more-results"), 2);
    CHECK(Odbc.SQLMoreResults(stmt.h) == SQL_NO_DATA);
    CHECK(cancelReachedServer(before));
  }
  // Re-executing on a statement whose previous result is still streaming
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(prepare(stmt.h, longResultSql("re-execute")), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    const std::optional<MockStats> before = mockStats();
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    CHECK(cancelReachedServer(before));
    ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  }
}

TEST_CASE("cancel/disconnect_and_free_env_mid_result_stop_server_query") {
  HARNESS_MOCK_ONLY();
  // SQLDisconnect with an open, still-streaming cursor (statement freed implicitly by the driver).
  {
    Env env;
    Dbc dbc(env);
    ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
    SQLHSTMT h = SQL_NULL_HSTMT;
    ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc.h, &h), SQL_HANDLE_DBC, dbc.h);
    const std::optional<MockStats> before = mockStats();
    openAndRead(h, longResultSql("disconnect"), 2);
    ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
    CHECK(cancelReachedServer(before));
  }
  // Two open cursors on one connection: both queries are stopped.
  {
    Env env;
    Dbc dbc(env);
    ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
    SQLHSTMT a = SQL_NULL_HSTMT, b = SQL_NULL_HSTMT;
    ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc.h, &a), SQL_HANDLE_DBC, dbc.h);
    ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc.h, &b), SQL_HANDLE_DBC, dbc.h);
    const std::optional<MockStats> before = mockStats();
    REQUIRE(before.has_value());
    openAndRead(a, longResultSql("disconnect-a"), 1);
    openAndRead(b, longResultSql("disconnect-b"), 1);
    ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
    const std::optional<MockStats> after =
        waitForStats([&](const MockStats& m) { return m.cancelled >= before->cancelled + 2; }, kCancelWaitMs);
    REQUIRE(after.has_value());
    CHECK(after->cancelled >= before->cancelled + 2);
  }
}

TEST_CASE("cancel/idle_fetch_timeout_cancels_abandoned_cursor") {
  Session s("IdleFetchTimeout=2");
  Stmt stmt(s.dbc);
  const std::optional<MockStats> before = mockStats();
  openAndRead(stmt.h, longResultSql("idle-timeout"), 1);
  // The application goes away without closing anything: after ~2 s the driver cancels the query
  // on its own (the timer does not need any further ODBC call).
  const double t0 = nowSeconds();
  if (ctx().mock) {
    REQUIRE(before.has_value());
    const std::optional<MockStats> after =
        waitForStats([&](const MockStats& m) { return m.cancelled > before->cancelled; }, 2000 + 2500);
    const double waited = nowSeconds() - t0;
    REQUIRE(after.has_value());
    CHECK(after->cancelled > before->cancelled);
    CHECK(waited >= 1.5);  // not before the timeout
    info("IdleFetchTimeout=2 cancelled the abandoned query after %.2f s", waited);
  } else {
    sleepMs(3500);  // the query state is checked through the server's query API by hand
  }
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY008");
  const std::vector<DiagRec> records = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!records.empty());
  CHECK(records[0].message.find("IdleFetchTimeout") != std::string::npos);
  // The statement is usable again after closing the cursor.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, smallTableSql(10)), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(10));
}

TEST_CASE("cancel/idle_fetch_timeout_spares_slow_but_active_reader") {
  HARNESS_MOCK_ONLY();
  Session s("AbandonedQueryTimeout=2");  // alias of IdleFetchTimeout
  Stmt stmt(s.dbc);
  openAndRead(stmt.h, longResultSql("idle-active"), 1);
  // One row every 400 ms for ~4 s: the rows come from pages already buffered (no page request),
  // yet every SQLFetch counts as activity, so the query must survive.
  for (int i = 0; i < 10; ++i) {
    sleepMs(400);
    ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  }
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(11));
  // A pure SQLGetData / SQLNumResultCols reader is active too.
  for (int i = 0; i < 6; ++i) {
    sleepMs(400);
    CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(5));
  }
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(12));
  // Off by default: a default connection idle for longer than that keeps its cursor.
  Session plain;
  Stmt other(plain.dbc);
  openAndRead(other.h, longResultSql("idle-off"), 1);
  sleepMs(2500);
  ODBC_REQUIRE(Odbc.SQLFetch(other.h), SQL_HANDLE_STMT, other.h);
  CHECK_EQ(getInt64(other.h, 1).value_or(-1), static_cast<int64_t>(2));
}
