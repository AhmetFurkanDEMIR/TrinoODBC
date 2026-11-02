// Error-path tests: invalid handles, function-sequence (HY010) and cursor-state (24000) rules,
// descriptor index / buffer length / C type / attribute validation, server error mapping, the
// SQLGetDiagField header and record fields, SQLGetDiagRec record ranges and message truncation,
// the ODBC 2 SQLError walk and conversion diagnostics during SQLGetData / SQLFetch.
#include "integration/odbc_loader.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifndef SQL_SETPARAM_VALUE_MAX
#  define SQL_SETPARAM_VALUE_MAX (-1L)
#endif

using namespace harness;

namespace {

/// SQLGetDiagFieldW string field as UTF-8 (byte convention). `rcOut` receives the return code.
std::string diagFieldString(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLSMALLINT field, SQLRETURN* rcOut = nullptr) {
  std::vector<SQLWCHAR> buf(1024);
  SQLSMALLINT lenBytes = 0;
  const SQLRETURN rc = Odbc.SQLGetDiagFieldW(type, h, rec, field, buf.data(),
                                             static_cast<SQLSMALLINT>(buf.size() * sizeof(SQLWCHAR)), &lenBytes);
  if (rcOut) *rcOut = rc;
  if (!SQL_SUCCEEDED(rc)) return std::string();
  return fromWide(buf.data(), lenBytes / static_cast<SQLSMALLINT>(sizeof(SQLWCHAR)));
}

template <typename T>
SQLRETURN diagFieldFixed(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLSMALLINT field, T& out) {
  out = T();
  SQLSMALLINT len = 0;
  return Odbc.SQLGetDiagFieldW(type, h, rec, field, &out, static_cast<SQLSMALLINT>(sizeof(T)), &len);
}

/// Provokes a server error (unknown table) on the statement and returns its return code.
SQLRETURN tableNotFound(SQLHSTMT stmt) { return execDirect(stmt, "SELECT * FROM no_such_table_xyz"); }

}  // namespace

TEST_CASE("errors/invalid_handles") {
  Session s;
  // Memory that is not a handle: rejected by the registry check, never dereferenced as a handle.
  alignas(16) unsigned char fake[512];
  std::memset(fake, 0, sizeof fake);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, fake) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DBC, fake) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_ENV, fake) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DESC, fake) == SQL_INVALID_HANDLE);
  WStr sql("SELECT 1");
  CHECK(Odbc.SQLExecDirectW(fake, sql.ptr(), sql.ilen()) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFetch(fake) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLDisconnect(fake) == SQL_INVALID_HANDLE);
  SQLWCHAR state[8];
  SQLINTEGER native = 0;
  SQLWCHAR msg[64];
  SQLSMALLINT len = 0;
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, fake, 1, state, &native, msg, 64, &len) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, fake, 0, SQL_DIAG_NUMBER, &native, sizeof native, &len) == SQL_INVALID_HANDLE);
  // Null handles.
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, SQL_NULL_HANDLE) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLExecDirectW(SQL_NULL_HSTMT, sql.ptr(), sql.ilen()) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLCancel(SQL_NULL_HSTMT) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLError(SQL_NULL_HENV, SQL_NULL_HDBC, SQL_NULL_HSTMT, nullptr, &native, nullptr, 0, &len) == SQL_INVALID_HANDLE);
  // Wrong handle type for a valid handle.
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_DBC, s.h(), 1, state, &native, msg, 64, &len) != SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, s.h(), 1, state, &native, msg, 64, &len) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, s.h()) == SQL_INVALID_HANDLE);
  // A freed statement handle is invalid afterwards (registry, not just the magic number).
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, s.h(), &stmt), SQL_HANDLE_DBC, s.h());
  ODBC_REQUIRE(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, stmt), SQL_HANDLE_STMT, stmt);
  CHECK(Odbc.SQLFreeStmt(stmt, SQL_CLOSE) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_STMT, stmt) == SQL_INVALID_HANDLE);
  CHECK(Odbc.SQLExecDirectW(stmt, sql.ptr(), sql.ilen()) == SQL_INVALID_HANDLE);
  // SQLFreeStmt(SQL_DROP) frees the handle as well.
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, s.h(), &stmt), SQL_HANDLE_DBC, s.h());
  ODBC_REQUIRE(Odbc.SQLFreeStmt(stmt, SQL_DROP), SQL_HANDLE_STMT, stmt);
  CHECK(Odbc.SQLFreeStmt(stmt, SQL_CLOSE) == SQL_INVALID_HANDLE);
  // Null output pointer.
  ODBC_CHECK_ERROR(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, s.h(), nullptr), SQL_HANDLE_DBC, s.h(), "HY009");
  CHECK(Odbc.SQLAllocHandle(99, s.h(), &stmt) != SQL_SUCCESS);
}

TEST_CASE("errors/free_handle_sequence_hy010") {
  SQLHENV env = SQL_NULL_HENV;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_HANDLE_ENV, env);
  ODBC_REQUIRE(Odbc.SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, attrVal(SQL_OV_ODBC3_80), 0), SQL_HANDLE_ENV, env);
  SQLHDBC dbc = SQL_NULL_HDBC;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc), SQL_HANDLE_ENV, env);
  // An environment with connections cannot be freed.
  ODBC_CHECK_ERROR(Odbc.SQLFreeHandle(SQL_HANDLE_ENV, env), SQL_HANDLE_ENV, env, "HY010");
  WStr cs(ctx().connStr);
  SQLSMALLINT outLen = 0;
  ODBC_REQUIRE(Odbc.SQLDriverConnectW(dbc, nullptr, cs.ptr(), cs.len(), nullptr, 0, &outLen, SQL_DRIVER_NOPROMPT), SQL_HANDLE_DBC, dbc);
  // A connected connection cannot be freed either.
  ODBC_CHECK_ERROR(Odbc.SQLFreeHandle(SQL_HANDLE_DBC, dbc), SQL_HANDLE_DBC, dbc, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLFreeConnect(dbc), SQL_HANDLE_DBC, dbc, "HY010");
  // The handle is still fully usable after the refused free.
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt), SQL_HANDLE_DBC, dbc);
  ODBC_CHECK(execDirect(stmt, "SELECT 1"), SQL_HANDLE_STMT, stmt);
  // Disconnecting with an allocated statement (and an open cursor) is allowed: it frees them.
  ODBC_CHECK(Odbc.SQLDisconnect(dbc), SQL_HANDLE_DBC, dbc);
  CHECK(Odbc.SQLFetch(stmt) == SQL_INVALID_HANDLE);
  ODBC_CHECK_ERROR(Odbc.SQLDisconnect(dbc), SQL_HANDLE_DBC, dbc, "08003");
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DBC, dbc), SQL_HANDLE_DBC, dbc);
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_ENV, env), SQL_HANDLE_ENV, env);
}

TEST_CASE("errors/function_sequence_hy010") {
  Session s;
  Stmt stmt(s.dbc);
  SQLSMALLINT n = 0;
  SQLLEN rows = 0;
  SQLPOINTER token = nullptr;
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLNumResultCols(stmt.h, &n), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLDescribeColW(stmt.h, 1, nullptr, 0, &n, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLColAttributeW(stmt.h, 1, SQL_DESC_NAME, nullptr, 0, &n, &rows), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLMoreResults(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLNumParams(stmt.h, &n), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLDescribeParam(stmt.h, 1, &n, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLParamData(stmt.h, &token), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLPutData(stmt.h, narrow("x"), 1), SQL_HANDLE_STMT, stmt.h, "HY010");
  // After a prepare, row-count style calls still need an execution.
  ODBC_REQUIRE(prepare(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLRowCount(stmt.h, &rows), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLMoreResults(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  // SQLExecute after SQLExecDirect: the statement was not prepared (ODBC S1 -> HY010).
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  // Environment attributes cannot change while connections exist.
  ODBC_CHECK_ERROR(Odbc.SQLSetEnvAttr(s.env.h, SQL_ATTR_ODBC_VERSION, attrVal(SQL_OV_ODBC3), 0), SQL_HANDLE_ENV, s.env.h, "HY010");
  // Empty statement text.
  ODBC_CHECK_ERROR(execDirect(stmt.h, "   "), SQL_HANDLE_STMT, stmt.h, "42000");
  ODBC_CHECK_ERROR(prepare(stmt.h, ""), SQL_HANDLE_STMT, stmt.h, "42000");
}

TEST_CASE("errors/cursor_state_24000") {
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER v = 0;
  SQLLEN ind = 0;
  ODBC_CHECK_ERROR(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");
  // Before any execution (S1) and for a prepared, unexecuted statement (S2/S3) the ODBC state
  // tables prescribe HY010 for SQLFetch / SQLGetData / SQLSetPos; 24000 is for executed statements.
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_REQUIRE(prepare(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK_ERROR(Odbc.SQLExtendedFetch(stmt.h, SQL_FETCH_NEXT, 0, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Cursor open but not positioned: SQLGetData / SQLSetPos are cursor-state errors.
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "24000");
  // Nothing may start a new execution while the cursor is open.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(prepare(stmt.h, "SELECT 2"), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(execDirect(stmt.h, "SELECT 2"), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_CURSOR_TYPE, attrVal(SQL_CURSOR_FORWARD_ONLY), 0), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_CONCURRENCY, attrVal(SQL_CONCUR_READ_ONLY), 0), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(Odbc.SQLSetScrollOptions(stmt.h, SQL_CONCUR_READ_ONLY, SQL_SCROLL_FORWARD_ONLY, 10), SQL_HANDLE_STMT, stmt.h, "24000");
  // The cursor itself is intact after those errors.
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // A prepared statement survives the close; the cursor is gone (back to S3: HY010).
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  // After SQL_NO_DATA the cursor is still open (SQLExecute -> 24000) until it is closed.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  // Executed without a result set (S4): now it is a cursor-state error.
  if (ctx().mock) {
    ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "24000");
    ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "24000");
    ODBC_CHECK_ERROR(Odbc.SQLSetPos(stmt.h, 1, SQL_POSITION, SQL_LOCK_NO_CHANGE), SQL_HANDLE_STMT, stmt.h, "24000");
  }
}

TEST_CASE("errors/descriptor_index_07009") {
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER v = 0;
  SQLLEN ind = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK_ERROR(Odbc.SQLBindCol(stmt.h, 0, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLDescribeColW(stmt.h, 0, nullptr, 0, &len, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLDescribeColW(stmt.h, 2, nullptr, 0, &len, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "07009");
  SQLLEN num = 0;
  ODBC_CHECK_ERROR(Odbc.SQLColAttributeW(stmt.h, 2, SQL_DESC_TYPE, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLColAttributeW(stmt.h, 0, SQL_DESC_TYPE, nullptr, 0, &len, &num), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 0, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 2, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "07009");
  // A binding beyond the result's columns is detected by the fetch.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 3, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "07009");
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Descriptor record 0 carries no data pointer.
  SQLHDESC ard = nullptr;
  SQLINTEGER ilen = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &ard, SQL_IS_POINTER, &ilen), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLSetDescFieldW(ard, 0, SQL_DESC_DATA_PTR, &v, SQL_IS_POINTER), SQL_HANDLE_DESC, ard, "07009");
  ODBC_CHECK_ERROR(Odbc.SQLGetDescFieldW(ard, -1, SQL_DESC_DATA_PTR, &v, SQL_IS_POINTER, &ilen), SQL_HANDLE_DESC, ard, "07009");
}

TEST_CASE("errors/buffer_length_and_c_type_validation") {
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER v = 0;
  char text[16];
  SQLLEN ind = 0;
  // HY090: negative buffer lengths.
  ODBC_CHECK_ERROR(Odbc.SQLBindCol(stmt.h, 1, SQL_C_CHAR, text, -1, &ind), SQL_HANDLE_STMT, stmt.h, "HY090");
  // HY003: unknown C types.
  ODBC_CHECK_ERROR(Odbc.SQLBindCol(stmt.h, 1, 9999, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "HY003");
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'abc'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_CHAR, text, -7, &ind), SQL_HANDLE_STMT, stmt.h, "HY090");
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, 9999, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "HY003");
  // The value is still readable after the rejected calls.
  CHECK_EQ(getString(stmt.h, 1), std::string("abc"));
  // Negative lengths on diagnostic reads are plain SQL_ERROR (nothing may be posted while reading).
  SQLCHAR state[8];
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  CHECK(Odbc.SQLGetDiagRec(SQL_HANDLE_STMT, stmt.h, 1, state, &native, reinterpret_cast<SQLCHAR*>(text), -1, &len) == SQL_ERROR);
  // HY092: unknown attributes / options, read-only attributes.
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, 99999, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h, "HY092");
  SQLINTEGER outLen = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetStmtAttrW(stmt.h, 99999, &v, sizeof v, &outLen), SQL_HANDLE_STMT, stmt.h, "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_NUMBER, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h, "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLFreeStmt(stmt.h, 77), SQL_HANDLE_STMT, stmt.h, "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), 99999, attrVal(1), 0), SQL_HANDLE_DBC, s.h(), "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLGetConnectAttrW(s.h(), 99999, &v, sizeof v, &outLen), SQL_HANDLE_DBC, s.h(), "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLSetEnvAttr(s.env.h, 99999, attrVal(1), 0), SQL_HANDLE_ENV, s.env.h, "HY092");
  // HY024: invalid attribute values.
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_NOSCAN, attrVal(77), 0), SQL_HANDLE_STMT, stmt.h, "HY024");
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_METADATA_ID, attrVal(77), 0), SQL_HANDLE_DBC, s.h(), "HY024");
  // HY009: null pointers where text is required.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLExecDirectW(stmt.h, nullptr, 0), SQL_HANDLE_STMT, stmt.h, "HY009");
  ODBC_CHECK_ERROR(Odbc.SQLPrepareW(stmt.h, nullptr, SQL_NTS), SQL_HANDLE_STMT, stmt.h, "HY009");
  ODBC_CHECK_ERROR(Odbc.SQLSetCursorNameW(stmt.h, nullptr, 0), SQL_HANDLE_STMT, stmt.h, "HY009");
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_CURRENT_CATALOG, nullptr, SQL_NTS), SQL_HANDLE_DBC, s.h(), "HY009");
  WStr in("SELECT 1");
  SQLINTEGER nativeLen = 0;
  ODBC_CHECK_ERROR(Odbc.SQLNativeSqlW(s.h(), nullptr, 0, nullptr, 0, &nativeLen), SQL_HANDLE_DBC, s.h(), "HY009");
  // HYC00 / HY106 for unsupported cursor operations.
  ODBC_CHECK_ERROR(Odbc.SQLBulkOperations(stmt.h, SQL_ADD), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLSetScrollOptions(stmt.h, SQL_CONCUR_LOCK, SQL_SCROLL_KEYSET_DRIVEN, 10), SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLFetchScroll(stmt.h, SQL_FETCH_LAST, 0), SQL_HANDLE_STMT, stmt.h, "HY106");
}

TEST_CASE("errors/server_error_mapping") {
  Session s;
  Stmt stmt(s.dbc);
  // Unknown table.
  ODBC_CHECK_ERROR(tableNotFound(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  std::vector<DiagRec> d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE_EQ(d.size(), static_cast<size_t>(1));
  CHECK(d[0].message.rfind("[Trino][ODBC Driver][Trino] ", 0) == 0);
  CHECK(d[0].message.find("no_such_table_xyz") != std::string::npos);
  if (ctx().mock) CHECK_EQ(d[0].native, static_cast<SQLINTEGER>(46));
  // The statement stays usable and the failed execution left no cursor behind: the failed
  // SQLExecDirect returned it to S1, where SQLFetch is a function sequence error.
  ODBC_CHECK_ERROR(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Syntax error.
  ODBC_CHECK_ERROR(execDirect(stmt.h, "SELEKT * FRUM nowhere"), SQL_HANDLE_STMT, stmt.h, "42000");
  d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!d.empty());
  if (ctx().mock) CHECK_EQ(d[0].native, static_cast<SQLINTEGER>(1));
  // A prepared statement with a bad table fails at describe time as well as at execute time.
  ODBC_CHECK(prepare(stmt.h, "SELECT * FROM no_such_table_xyz"), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT n = 0;
  ODBC_CHECK_ERROR(Odbc.SQLNumResultCols(stmt.h, &n), SQL_HANDLE_STMT, stmt.h, "42S02");
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  if (!ctx().mock) return;
  ODBC_CHECK_ERROR(execDirect(stmt.h, "SELECT nosuch FROM types_demo"), SQL_HANDLE_STMT, stmt.h, "42S22");
  ODBC_CHECK_ERROR(execDirect(stmt.h, "USE nosuchschema"), SQL_HANDLE_STMT, stmt.h, "3F000");
  // HTTP 403 on the statement request maps to an authorization failure.
  ODBC_CHECK_ERROR(execDirect(stmt.h, "SELECT * FROM auth_table"), SQL_HANDLE_STMT, stmt.h, "28000");
  d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!d.empty());
  CHECK(d[0].message.rfind("[Trino][ODBC Driver] ", 0) == 0);
  // Failed EXECUTE of a prepared statement the server does not know (explicit prepare, unknown name).
  Session explicitPrepare("ExplicitPrepare=1");
  Stmt ex(explicitPrepare.dbc);
  ODBC_REQUIRE(prepare(ex.h, "SELECT ? AS p1"), SQL_HANDLE_STMT, ex.h);
  SQLINTEGER v = 1;
  SQLLEN ind = 0;
  ODBC_REQUIRE(Odbc.SQLBindParameter(ex.h, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind), SQL_HANDLE_STMT, ex.h);
  ODBC_CHECK(Odbc.SQLExecute(ex.h), SQL_HANDLE_STMT, ex.h);
}

TEST_CASE("errors/diag_field_header_fields") {
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER number = -1;
  SQLRETURN stored = -1;
  SQLLEN rowCount = -99;
  SQLINTEGER code = -1;
  // Success: no records, return code recorded, row count for DML.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(number, static_cast<SQLINTEGER>(0));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_RETURNCODE, stored), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(stored, static_cast<SQLRETURN>(SQL_SUCCESS));
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION), std::string("SELECT CURSOR"));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION_CODE, code), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(code, static_cast<SQLINTEGER>(SQL_DIAG_SELECT_CURSOR));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_CURSOR_ROW_COUNT, rowCount), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));  // forward-only: unknown
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) {
    ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_ROW_COUNT, rowCount), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(rowCount, static_cast<SQLLEN>(3));
    CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION), std::string("INSERT"));
    ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION_CODE, code), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(code, static_cast<SQLINTEGER>(SQL_DIAG_INSERT));
    ODBC_REQUIRE(execDirect(stmt.h, "CREATE TABLE t9 (a bigint)"), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION), std::string("CREATE TABLE"));
    ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_DYNAMIC_FUNCTION_CODE, code), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(code, static_cast<SQLINTEGER>(SQL_DIAG_CREATE_TABLE));
  }
  // Error: one record, return code SQL_ERROR.
  ODBC_CHECK_ERROR(tableNotFound(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(number, static_cast<SQLINTEGER>(1));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_RETURNCODE, stored), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(stored, static_cast<SQLRETURN>(SQL_ERROR));
  // Reading diagnostics does not clear them.
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(number, static_cast<SQLINTEGER>(1));
  // Statement-only header fields are errors on other handle types; SQL_DIAG_NUMBER works everywhere.
  CHECK(diagFieldFixed(SQL_HANDLE_DBC, s.h(), 0, SQL_DIAG_ROW_COUNT, rowCount) == SQL_ERROR);
  CHECK(diagFieldFixed(SQL_HANDLE_DBC, s.h(), 0, SQL_DIAG_CURSOR_ROW_COUNT, rowCount) == SQL_ERROR);
  CHECK(diagFieldFixed(SQL_HANDLE_ENV, s.env.h, 0, SQL_DIAG_ROW_COUNT, rowCount) == SQL_ERROR);
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_DBC, s.h(), 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_ENV, s.env.h, 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_ENV, s.env.h);
  CHECK_EQ(number, static_cast<SQLINTEGER>(0));
  // Unknown field identifier.
  CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, 32000, number) == SQL_ERROR);
  // The area of a fresh call is cleared: a successful call afterwards reports zero records.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_NUMBER, number), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(number, static_cast<SQLINTEGER>(0));
}

TEST_CASE("errors/diag_field_record_fields") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_CHECK_ERROR(tableNotFound(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  SQLRETURN rc = SQL_SUCCESS;
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SQLSTATE, &rc), std::string("42S02"));
  ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_CLASS_ORIGIN), std::string("ISO 9075"));
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SUBCLASS_ORIGIN), std::string("ODBC 3.0"));
  const std::string message = diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_MESSAGE_TEXT);
  CHECK(message.rfind("[Trino][ODBC Driver][Trino] ", 0) == 0);
  const std::string firstRecord = diag(SQL_HANDLE_STMT, stmt.h)[0].message;
  CHECK_EQ(message, firstRecord);
  SQLINTEGER native = 0;
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_NATIVE, native), SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) CHECK_EQ(native, static_cast<SQLINTEGER>(46));
  const std::string server = diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SERVER_NAME, &rc);
  ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt.h);
  if (ctx().mock) CHECK_EQ(server, ctx().host);
  diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_CONNECTION_NAME, &rc);
  ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt.h);
  SQLLEN rowNumber = 0;
  SQLINTEGER column = 0;
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_ROW_NUMBER, rowNumber), SQL_HANDLE_STMT, stmt.h);
  CHECK(rowNumber < 1);  // SQL_NO_ROW_NUMBER / SQL_ROW_NUMBER_UNKNOWN: the error is not tied to a row
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_COLUMN_NUMBER, column), SQL_HANDLE_STMT, stmt.h);
  CHECK(column < 1);
  // Record ranges: 0 is an error, past the end is SQL_NO_DATA.
  CHECK(Odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, stmt.h, 0, SQL_DIAG_SQLSTATE, nullptr, 0, nullptr) == SQL_ERROR);
  CHECK(Odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, stmt.h, 2, SQL_DIAG_SQLSTATE, nullptr, 0, nullptr) == SQL_NO_DATA);
  CHECK(Odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, stmt.h, -1, SQL_DIAG_SQLSTATE, nullptr, 0, nullptr) == SQL_ERROR);
  // Driver-defined classes report "ODBC 3.0" for both origins.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_CLASS_ORIGIN), std::string("ODBC 3.0"));
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SUBCLASS_ORIGIN), std::string("ODBC 3.0"));
  CHECK(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_MESSAGE_TEXT).rfind("[Trino][ODBC Driver] ", 0) == 0);
  // Narrow SQLGetDiagField reports the same text in bytes.
  char narrowState[16] = {0};
  SQLSMALLINT len = 0;
  ODBC_CHECK(Odbc.SQLGetDiagField(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SQLSTATE, narrowState, sizeof narrowState, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(std::string(narrowState), std::string("HY010"));
  CHECK_EQ(static_cast<int>(len), 5);
  // Column number of a conversion error during SQLGetData.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'abc'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER v = 0;
  SQLLEN ind = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &v, sizeof v, &ind), SQL_HANDLE_STMT, stmt.h, "22018");
  CHECK_EQ(diagFieldString(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_SUBCLASS_ORIGIN), std::string("ISO 9075"));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_COLUMN_NUMBER, column), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(column, static_cast<SQLINTEGER>(1));
  ODBC_CHECK(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, 1, SQL_DIAG_ROW_NUMBER, rowNumber), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowNumber, static_cast<SQLLEN>(1));
}

TEST_CASE("errors/diag_rec_ranges_and_truncation") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_CHECK_ERROR(tableNotFound(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  const std::string full = diag(SQL_HANDLE_STMT, stmt.h)[0].message;
  REQUIRE(full.size() > 20);
  SQLWCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLWCHAR msg[11];
  SQLSMALLINT len = 0;
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, stmt.h, 0, state, &native, msg, 11, &len) == SQL_ERROR);
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, stmt.h, -1, state, &native, msg, 11, &len) == SQL_ERROR);
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, stmt.h, 2, state, &native, msg, 11, &len) == SQL_NO_DATA);
  // Truncated message: 01004-style return, full length reported in characters, NUL-terminated.
  const SQLRETURN rc = Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, stmt.h, 1, state, &native, msg, 11, &len);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(fromWide(state, 5), std::string("42S02"));
  CHECK_EQ(static_cast<SQLLEN>(len), wideLength(full));
  CHECK(msg[10] == 0);
  CHECK_EQ(fromWide(msg, 10), fromWide(toWide(full).data(), 10));
  // Length-only query with a null message buffer.
  SQLSMALLINT lenOnly = 0;
  const SQLRETURN rc2 = Odbc.SQLGetDiagRecW(SQL_HANDLE_STMT, stmt.h, 1, state, &native, nullptr, 0, &lenOnly);
  CHECK(SQL_SUCCEEDED(rc2));
  CHECK_EQ(static_cast<SQLLEN>(lenOnly), wideLength(full));
  // The truncated read did not disturb the diagnostic area.
  CHECK_EQ(diag(SQL_HANDLE_STMT, stmt.h).size(), static_cast<size_t>(1));
  // ANSI variant: bytes, 6-byte SQLSTATE buffer.
  SQLCHAR astate[6] = {0};
  SQLCHAR amsg[8] = {0};
  const SQLRETURN rc3 = Odbc.SQLGetDiagRec(SQL_HANDLE_STMT, stmt.h, 1, astate, &native, amsg, 8, &len);
  CHECK(rc3 == SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(std::string(reinterpret_cast<char*>(astate)), std::string("42S02"));
  CHECK_EQ(std::string(reinterpret_cast<char*>(amsg)), full.substr(0, 7));
  CHECK_EQ(static_cast<size_t>(len), full.size());
  SQLCHAR big[1024];
  ODBC_CHECK(Odbc.SQLGetDiagRec(SQL_HANDLE_STMT, stmt.h, 1, astate, &native, big, sizeof big, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(std::string(reinterpret_cast<char*>(big)), full);
  // Records are ordered errors first, then warnings (01004 from a truncating fetch comes after).
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'abcdef'"), SQL_HANDLE_STMT, stmt.h);
  char small[3];
  SQLLEN ind = 0;
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 1, SQL_C_CHAR, small, sizeof small, &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_SUCCESS_WITH_INFO);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_STMT, stmt.h);
  REQUIRE(!d.empty());
  CHECK_EQ(d[0].sqlstate, std::string("01004"));
  CHECK_EQ(std::string(small), std::string("ab"));
  CHECK_EQ(ind, static_cast<SQLLEN>(6));
}

TEST_CASE("errors/sqlerror_odbc2_walk") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_CHECK_ERROR(tableNotFound(stmt.h), SQL_HANDLE_STMT, stmt.h, "42S02");
  SQLCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLCHAR msg[512] = {0};
  SQLSMALLINT len = 0;
  // The most specific handle wins: the statement's record comes back once, then SQL_NO_DATA.
  ODBC_CHECK(Odbc.SQLError(s.env.h, s.h(), stmt.h, state, &native, msg, sizeof msg, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(std::string(reinterpret_cast<char*>(state)), std::string("42S02"));
  CHECK(std::string(reinterpret_cast<char*>(msg)).find("no_such_table_xyz") != std::string::npos);
  CHECK(Odbc.SQLError(s.env.h, s.h(), stmt.h, state, &native, msg, sizeof msg, &len) == SQL_NO_DATA);
  CHECK_EQ(std::string(reinterpret_cast<char*>(state)), std::string("00000"));
  CHECK_EQ(msg[0], static_cast<SQLCHAR>(0));
  CHECK_EQ(native, static_cast<SQLINTEGER>(0));
  // SQLGetDiagRec is unaffected by the walk.
  CHECK_EQ(diag(SQL_HANDLE_STMT, stmt.h).size(), static_cast<size_t>(1));
  // A new call resets the walk position.
  ODBC_CHECK_ERROR(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h, "HY010");
  ODBC_CHECK(Odbc.SQLError(SQL_NULL_HENV, SQL_NULL_HDBC, stmt.h, state, &native, msg, sizeof msg, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(std::string(reinterpret_cast<char*>(state)), std::string("HY010"));
  CHECK(Odbc.SQLError(SQL_NULL_HENV, SQL_NULL_HDBC, stmt.h, state, &native, msg, sizeof msg, &len) == SQL_NO_DATA);
  // Connection-level walk through the wide variant.
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), 99999, attrVal(1), 0), SQL_HANDLE_DBC, s.h(), "HY092");
  SQLWCHAR wstate[6] = {0};
  SQLWCHAR wmsg[512] = {0};
  ODBC_CHECK(Odbc.SQLErrorW(s.env.h, s.h(), SQL_NULL_HSTMT, wstate, &native, wmsg, 512, &len), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(fromWide(wstate, 5), std::string("HY092"));
  CHECK(Odbc.SQLErrorW(s.env.h, s.h(), SQL_NULL_HSTMT, wstate, &native, wmsg, 512, &len) == SQL_NO_DATA);
  // Environment-level with nothing to report.
  CHECK(Odbc.SQLError(s.env.h, SQL_NULL_HDBC, SQL_NULL_HSTMT, state, &native, msg, sizeof msg, &len) == SQL_NO_DATA);
}

TEST_CASE("errors/conversion_diagnostics_getdata_and_fetch") {
  Session s;
  Stmt stmt(s.dbc);
  SQLLEN ind = 0;
  // 22003: value does not fit the target.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 2147483647"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  SQLSCHAR tiny = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_STINYINT, &tiny, sizeof tiny, &ind), SQL_HANDLE_STMT, stmt.h, "22003");
  SQLSMALLINT small = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SSHORT, &small, sizeof small, &ind), SQL_HANDLE_STMT, stmt.h, "22003");
  unsigned char bit = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_BIT, &bit, sizeof bit, &ind), SQL_HANDLE_STMT, stmt.h, "22003");
  // After a failed conversion the value is still available in a fitting type.
  SQLINTEGER whole = 0;
  ODBC_CHECK(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &whole, sizeof whole, &ind), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(whole, static_cast<SQLINTEGER>(2147483647));
  // 07006: an integer cannot become a date or a GUID.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  SQL_DATE_STRUCT date;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_TYPE_DATE, &date, sizeof date, &ind), SQL_HANDLE_STMT, stmt.h, "07006");
  SQLGUID guid;
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_GUID, &guid, sizeof guid, &ind), SQL_HANDLE_STMT, stmt.h, "07006");
  // 22018: text that is not a number; 01S07: a fraction dropped on the way to an integer.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'abc'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &whole, sizeof whole, &ind), SQL_HANDLE_STMT, stmt.h, "22018");
  ODBC_CHECK_ERROR(Odbc.SQLGetData(stmt.h, 1, SQL_C_TYPE_TIMESTAMP, &date, sizeof(SQL_TIMESTAMP_STRUCT), &ind), SQL_HANDLE_STMT, stmt.h, "22018");
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT '3.5'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const SQLRETURN rc = Odbc.SQLGetData(stmt.h, 1, SQL_C_SLONG, &whole, sizeof whole, &ind);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01S07");
  CHECK_EQ(whole, static_cast<SQLINTEGER>(3));
  // A bound column that fails to convert makes SQLFetch fail for that row: 01S01 + the cause.
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SLONG, &whole, sizeof whole, &ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 'abc'"), SQL_HANDLE_STMT, stmt.h);
  const SQLRETURN frc = Odbc.SQLFetch(stmt.h);
  CHECK(frc == SQL_ERROR);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_STMT, stmt.h);
  CHECK(hasState(d, "22018"));
  CHECK(hasState(d, "01S01"));
  // A rowset where only some rows fail returns SQL_SUCCESS_WITH_INFO with per-row status.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  if (!ctx().mock) return;
  SQLINTEGER ints[3] = {0, 0, 0};
  SQLLEN inds[3] = {0, 0, 0};
  SQLUSMALLINT status[3] = {0xFFFF, 0xFFFF, 0xFFFF};
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(3), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_STATUS_PTR, status, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  // c_real: 1.5 (01S07 fraction), NULL, NaN (22003) -> row statuses differ.
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 6, SQL_C_SLONG, ints, sizeof(SQLINTEGER), inds), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM types_demo"), SQL_HANDLE_STMT, stmt.h);
  const SQLRETURN brc = Odbc.SQLFetch(stmt.h);
  CHECK(brc == SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(fetched, static_cast<SQLULEN>(3));
  CHECK_EQ(status[0], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS_WITH_INFO));
  CHECK_EQ(ints[0], static_cast<SQLINTEGER>(1));
  CHECK_EQ(status[1], static_cast<SQLUSMALLINT>(SQL_ROW_SUCCESS));
  CHECK_EQ(inds[1], static_cast<SQLLEN>(SQL_NULL_DATA));
  CHECK_EQ(status[2], static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
  const std::vector<DiagRec> bd = diag(SQL_HANDLE_STMT, stmt.h);
  CHECK(hasState(bd, "01S07"));
  CHECK(hasState(bd, "22003"));
  CHECK(hasState(bd, "01S01"));
  // Row numbers in the records point at the failing rows of the rowset.
  bool sawRow3 = false;
  for (SQLSMALLINT rec = 1; rec <= static_cast<SQLSMALLINT>(bd.size()); ++rec) {
    SQLLEN row = 0;
    if (SQL_SUCCEEDED(diagFieldFixed(SQL_HANDLE_STMT, stmt.h, rec, SQL_DIAG_ROW_NUMBER, row)) && row == 3) sawRow3 = true;
  }
  CHECK(sawRow3);
}

// ---- parameter binding through driver-manager mappings, IPD, cursor names, transactions ---------

namespace {

/// SQL_ATTR_IMP_PARAM_DESC / SQL_ATTR_APP_PARAM_DESC handle of a statement.
SQLHDESC stmtDesc(SQLHSTMT stmt, SQLINTEGER attr) {
  SQLHDESC d = SQL_NULL_HDESC;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt, attr, &d, sizeof d, nullptr), SQL_HANDLE_STMT, stmt);
  return d;
}

SQLLEN descCount(SQLHDESC d) {
  SQLSMALLINT n = -1;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(d, 0, SQL_DESC_COUNT, &n, sizeof n, nullptr), SQL_HANDLE_DESC, d);
  return n;
}

/// Integer member of /mock/stats (0 when unavailable).
int64_t mockCounter(const char* key) {
  const std::optional<std::string> body = httpGet(ctx().host, ctx().port, "/mock/stats");
  return body ? jsonInt(*body, key).value_or(0) : 0;
}

std::string setCursorName(SQLHSTMT stmt, const std::string& name, SQLRETURN* rc) {
  *rc = Odbc.SQLSetCursorName(stmt, narrow(name), static_cast<SQLSMALLINT>(name.size()));
  return firstState(SQL_HANDLE_STMT, stmt);
}

}  // namespace

TEST_CASE("errors/bindparameter_accepts_setparam_mapping") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT ?"), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER v = 42;
  SQLLEN ind = 0;
  // What a driver manager sends for ODBC 2 SQLSetParam ("SQLSetParam Mapping"): INPUT_OUTPUT with
  // BufferLength = SQL_SETPARAM_VALUE_MAX. It is an input parameter.
  ODBC_REQUIRE(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT_OUTPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v,
                                     SQL_SETPARAM_VALUE_MAX, &ind),
               SQL_HANDLE_STMT, stmt.h);
  ind = sizeof v;
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK(getString(stmt.h, 1).find("42") != std::string::npos);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // The IPD describes it as an input parameter.
  SQLHDESC ipd = stmtDesc(stmt.h, SQL_ATTR_IMP_PARAM_DESC);
  SQLSMALLINT ioType = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 1, SQL_DESC_PARAMETER_TYPE, &ioType, sizeof ioType, nullptr), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(ioType, static_cast<SQLSMALLINT>(SQL_PARAM_INPUT));
  // ISO SQLBindParam mapping: SQL_PARAM_INPUT with SQL_SETPARAM_VALUE_MAX.
  char text[] = "abc";
  SQLLEN nts = SQL_NTS;
  ODBC_REQUIRE(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 10, 0, text,
                                     SQL_SETPARAM_VALUE_MAX, &nts),
               SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLExecute(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK(getString(stmt.h, 1).find("abc") != std::string::npos);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Real input/output parameters and other negative lengths are still rejected.
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT_OUTPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind),
                   SQL_HANDLE_STMT, stmt.h, "HYC00");
  ODBC_CHECK_ERROR(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 10, 0, text, -2, &nts),
                   SQL_HANDLE_STMT, stmt.h, "HY090");
}

TEST_CASE("errors/ipd_describes_parameters_at_execute") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(prepare(stmt.h, "SELECT ?"), SQL_HANDLE_STMT, stmt.h);
  char dec[] = "1.2345";
  SQLLEN nts = SQL_NTS;
  ODBC_REQUIRE(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_DECIMAL, 18, 2, dec, 0, &nts),
               SQL_HANDLE_STMT, stmt.h);
  // SQL_DESC_SCALE of the IPD is the authoritative scale of the parameter.
  SQLHDESC ipd = stmtDesc(stmt.h, SQL_ATTR_IMP_PARAM_DESC);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd, 1, SQL_DESC_SCALE, attrVal(4), 0), SQL_HANDLE_DESC, ipd);
  const SQLRETURN rc = Odbc.SQLExecute(stmt.h);
  ODBC_REQUIRE(rc, SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  const std::string literal = getString(stmt.h, 1);
  CHECK(literal.find("1.2345") != std::string::npos);
  CHECK_EQ(rc, static_cast<SQLRETURN>(SQL_SUCCESS));  // nothing was cut to scale 2
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_RESET_PARAMS), SQL_HANDLE_STMT, stmt.h);

  // A parameter bound through descriptors only: APD data pointer + IPD SQL type.
  Stmt d(s.dbc);
  SQLHDESC apd = stmtDesc(d.h, SQL_ATTR_APP_PARAM_DESC);
  SQLHDESC ipd2 = stmtDesc(d.h, SQL_ATTR_IMP_PARAM_DESC);
  char date[] = "2024-01-02";
  SQLLEN dateLen = SQL_NTS;
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd2, 1, SQL_DESC_CONCISE_TYPE, attrVal(SQL_TYPE_DATE), 0), SQL_HANDLE_DESC, ipd2);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(apd, 1, SQL_DESC_CONCISE_TYPE, attrVal(SQL_C_CHAR), 0), SQL_HANDLE_DESC, apd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(apd, 1, SQL_DESC_OCTET_LENGTH, attrVal(sizeof date), 0), SQL_HANDLE_DESC, apd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(apd, 1, SQL_DESC_INDICATOR_PTR, &dateLen, 0), SQL_HANDLE_DESC, apd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, &dateLen, 0), SQL_HANDLE_DESC, apd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(apd, 1, SQL_DESC_DATA_PTR, date, 0), SQL_HANDLE_DESC, apd);
  ODBC_REQUIRE(execDirect(d.h, "SELECT ?"), SQL_HANDLE_STMT, d.h);
  ODBC_REQUIRE(Odbc.SQLFetch(d.h), SQL_HANDLE_STMT, d.h);
  const std::string dateLiteral = getString(d.h, 1);
  CHECK(dateLiteral.find("DATE '2024-01-02'") != std::string::npos);
  if (dateLiteral.find("DATE '2024-01-02'") == std::string::npos) info("rendered: %s", dateLiteral.c_str());
}

TEST_CASE("errors/ipd_records_survive_prepare_and_reset_params") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  SQLINTEGER v = 7;
  SQLLEN ind = 0;
  ODBC_REQUIRE(Odbc.SQLBindParameter(stmt.h, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &v, 0, &ind),
               SQL_HANDLE_STMT, stmt.h);
  SQLHDESC ipd = stmtDesc(stmt.h, SQL_ATTR_IMP_PARAM_DESC);
  SQLHDESC apd = stmtDesc(stmt.h, SQL_ATTR_APP_PARAM_DESC);
  // Bindings persist across prepares: so does the IPD record SQLBindParameter wrote.
  ODBC_REQUIRE(prepare(stmt.h, "SELECT ?"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(descCount(ipd), static_cast<SQLLEN>(1));
  SQLSMALLINT type = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 1, SQL_DESC_CONCISE_TYPE, &type, sizeof type, nullptr), SQL_HANDLE_DESC, ipd);
  CHECK_EQ(type, static_cast<SQLSMALLINT>(SQL_INTEGER));
  ODBC_REQUIRE(prepare(stmt.h, "SELECT ? /* again */"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(descCount(ipd), static_cast<SQLLEN>(1));
  // SQL_RESET_PARAMS only sets the APD count to 0.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_RESET_PARAMS), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(descCount(apd), static_cast<SQLLEN>(0));
  CHECK_EQ(descCount(ipd), static_cast<SQLLEN>(1));
  // Auto-populated IPD records (DESCRIBE INPUT) survive SQL_RESET_PARAMS as well.
  Stmt other(s.dbc);
  ODBC_REQUIRE(prepare(other.h, "SELECT ?"), SQL_HANDLE_STMT, other.h);
  SQLSMALLINT sqlType = 0;
  SQLULEN size = 0;
  SQLSMALLINT digits = 0, nullable = 0;
  ODBC_REQUIRE(Odbc.SQLDescribeParam(other.h, 1, &sqlType, &size, &digits, &nullable), SQL_HANDLE_STMT, other.h);
  SQLHDESC ipd2 = stmtDesc(other.h, SQL_ATTR_IMP_PARAM_DESC);
  const SQLLEN described = descCount(ipd2);
  ODBC_CHECK(Odbc.SQLFreeStmt(other.h, SQL_RESET_PARAMS), SQL_HANDLE_STMT, other.h);
  CHECK_EQ(descCount(ipd2), described);
}

TEST_CASE("errors/cursor_name_reserved_prefixes_and_duplicates") {
  Session s;
  Stmt a(s.dbc);
  Stmt b(s.dbc);
  SQLRETURN rc = SQL_SUCCESS;
  // Default names are SQL_CUR<n>; the SQLCUR / SQL_CUR prefixes are reserved (34000).
  CHECK_EQ(setCursorName(b.h, "SQL_CUR1", &rc), std::string("34000"));
  CHECK(rc == SQL_ERROR);
  CHECK_EQ(setCursorName(b.h, "sqlcur_mine", &rc), std::string("34000"));
  CHECK(rc == SQL_ERROR);
  CHECK_EQ(setCursorName(b.h, "Sql_Cur7", &rc), std::string("34000"));
  CHECK(rc == SQL_ERROR);
  // Names are unique per connection, case-insensitively (3C000); re-setting one's own name is fine.
  setCursorName(a.h, "orders_cursor", &rc);
  CHECK(rc == SQL_SUCCESS);
  CHECK_EQ(setCursorName(b.h, "ORDERS_CURSOR", &rc), std::string("3C000"));
  CHECK(rc == SQL_ERROR);
  setCursorName(a.h, "orders_cursor", &rc);
  CHECK(rc == SQL_SUCCESS);
  setCursorName(b.h, "lines_cursor", &rc);
  CHECK(rc == SQL_SUCCESS);
}

TEST_CASE("errors/driver_transaction_starts_with_start_transaction") {
  HARNESS_MOCK_ONLY();
  Session s("EnableTransactions=1");
  Stmt stmt(s.dbc);
  const int64_t started0 = mockCounter("transactions_started");
  const int64_t inTxn0 = mockCounter("transactional_statements");
  const int64_t rollbacks0 = mockCounter("rollbacks");
  const int64_t commits0 = mockCounter("commits");
  // Autocommit: no transaction is opened.
  ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (0)"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(mockCounter("transactions_started"), started0);

  ODBC_REQUIRE(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_AUTOCOMMIT, attrVal(SQL_AUTOCOMMIT_OFF), 0), SQL_HANDLE_DBC, s.h());
  // The first statement opens the transaction with START TRANSACTION and then runs inside it (a
  // statement merely sent with X-Trino-Transaction-Id: NONE would autocommit in Trino).
  ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(mockCounter("transactions_started"), started0 + 1);
  CHECK_EQ(mockCounter("transactional_statements"), inTxn0 + 1);
  ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (2)"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(mockCounter("transactions_started"), started0 + 1);
  CHECK_EQ(mockCounter("transactional_statements"), inTxn0 + 2);
  // SQLEndTran really rolls it back.
  ODBC_REQUIRE(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_ROLLBACK), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(mockCounter("rollbacks"), rollbacks0 + 1);
  // The next statement opens a new transaction, which COMMIT ends.
  ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (3)"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(mockCounter("transactions_started"), started0 + 2);
  ODBC_REQUIRE(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(mockCounter("commits"), commits0 + 1);
  // Nothing open: SQLEndTran has nothing to send.
  ODBC_REQUIRE(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(mockCounter("commits"), commits0 + 1);
  ODBC_REQUIRE(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_AUTOCOMMIT, attrVal(SQL_AUTOCOMMIT_ON), 0), SQL_HANDLE_DBC, s.h());
}
