#pragma once
// Shared plumbing for the exported ODBC entry points:
//  * handle validation + locking + exception-to-diagnostic mapping (guard*)
//  * TextIn / TextOut: encoding-agnostic string arguments so that ANSI and Unicode entry points share
//    one implementation (namespace impl)
// Every src/odbc/api_*.cpp file includes this header. api_unicode.cpp contains all *W entry points.
#include "platform.h"
// <windows.h> (wingdi.h) defines TextOut as a macro for the GDI function; it would rename the class
// below and make every unqualified use at global scope ambiguous with ::TextOutA on Windows.
#ifdef TextOut
#  undef TextOut
#endif
#include "catalog/catalog.h"
#include "core/diag.h"
#include "core/handles.h"
#include "util/log.h"
#include "util/unicode.h"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace trino_odbc {

// ---- Input text -------------------------------------------------------------------------------
/// Converts an ODBC input string argument to UTF-8. `length` is SQL_NTS or a count of characters
/// (SQLCHAR bytes / SQLWCHAR units). A null pointer yields nullopt (distinct from empty string).
std::optional<std::string> textIn(const SQLCHAR* s, SQLLEN length, AnsiEncoding enc);
std::optional<std::string> textIn(const SQLWCHAR* s, SQLLEN length);
/// Same, but for attribute values whose length is given in BYTES for wide strings (SQLSetConnectAttrW
/// with SQL_ATTR_CURRENT_CATALOG etc.). `lengthBytes` may be SQL_NTS.
std::optional<std::string> textInBytes(const SQLPOINTER s, SQLINTEGER lengthBytes, bool wide, AnsiEncoding enc);

// ---- Output text ------------------------------------------------------------------------------
/// Destination for an ODBC output string. `lengthInBytes` selects the SQLGetInfoW/SQLColAttributeW
/// byte convention vs the character convention used by most other functions.
class TextOut {
 public:
  /// Narrow buffer (bytes).
  TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLSMALLINT* outLen, AnsiEncoding enc);
  TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLINTEGER* outLen, AnsiEncoding enc);
#if !defined(_WIN32) || defined(_WIN64)  // on 32-bit Windows SQLLEN is SQLINTEGER (same overload)
  TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLLEN* outLen, AnsiEncoding enc);
#endif
  /// Wide buffer; `bufLen` and `*outLen` in SQLWCHAR units unless `lengthInBytes`.
  TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLSMALLINT* outLen, bool lengthInBytes);
  TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLINTEGER* outLen, bool lengthInBytes);
#if !defined(_WIN32) || defined(_WIN64)
  TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLLEN* outLen, bool lengthInBytes);
#endif
  /// Discarding sink (argument not requested).
  TextOut();

  /// Writes `utf8`; returns true when truncated (caller adds 01004 unless it prefers another state).
  bool write(std::string_view utf8);
  /// Writes and adds 01004 to `diag` on truncation; returns SQL_SUCCESS / SQL_SUCCESS_WITH_INFO.
  SQLRETURN writeWithDiag(std::string_view utf8, Diag& diag);
  bool wide() const { return wide_; }
  SQLLEN bufferLength() const { return bufLen_; }
  bool hasBuffer() const { return buf_ != nullptr; }

 private:
  void setOutLen(SQLLEN v);
  void* buf_ = nullptr;
  SQLLEN bufLen_ = 0;
  bool wide_ = false;
  bool lengthInBytes_ = false;
  AnsiEncoding enc_ = AnsiEncoding::Utf8;
  SQLSMALLINT* out16_ = nullptr;
  SQLINTEGER* out32_ = nullptr;
  SQLLEN* outLen_ = nullptr;
};

// ---- Guards -----------------------------------------------------------------------------------
// Each guard: validates the handle (SQL_INVALID_HANDLE), locks its mutex, clears the diagnostic area
// (unless keepDiag), runs `body`, converts exceptions into diagnostics, stores diag.returnCode and
// traces the call. `body` receives the typed handle and returns SQLRETURN.
SQLRETURN guardEnv(SQLHENV h, const char* fn, const std::function<SQLRETURN(Environment&)>& body, bool keepDiag = false);
SQLRETURN guardDbc(SQLHDBC h, const char* fn, const std::function<SQLRETURN(Connection&)>& body, bool keepDiag = false);
SQLRETURN guardStmt(SQLHSTMT h, const char* fn, const std::function<SQLRETURN(Statement&)>& body, bool keepDiag = false);
SQLRETURN guardDesc(SQLHDESC h, const char* fn, const std::function<SQLRETURN(Descriptor&)>& body, bool keepDiag = false);
/// Resolves the Diag of any handle type (for SQLGetDiagRec/Field, SQLError). Returns nullptr when invalid.
Diag* diagFor(SQLSMALLINT handleType, SQLHANDLE handle);
/// Locks the mutex that protects the diag of the given handle (descriptors lock their connection).
std::recursive_mutex* mutexFor(SQLSMALLINT handleType, SQLHANDLE handle);

/// Converts a caught exception into diagnostics on `diag` and returns the SQLRETURN.
SQLRETURN diagFromException(Diag& diag, const std::exception& e);

/// SQL_DBMS_VER text ("483.00.0000 Trino 483", or "00.00.0000" when unknown) — api_info.cpp.
std::string dbmsVersionString(const Connection& c);

// ---- State checks ---------------------------------------------------------------------------
/// Throws DriverError(HY010) unless the connection is connected.
void requireConnected(const Connection& c);
/// Throws DriverError(HY010) when the statement is in the NeedData or Executing state.
void requireNotBusy(const Statement& s);
/// Throws DriverError(24000) unless a cursor is open (CursorOpen/Fetched).
void requireCursor(const Statement& s);
/// Throws DriverError(24000) when a cursor is open (for SQLPrepare/SQLExecute while results pending).
void requireNoCursor(const Statement& s);
/// AnsiEncoding effective for a handle.
AnsiEncoding ansiEncodingOf(const Connection& c);
AnsiEncoding ansiEncodingOf(const Statement& s);
/// Bytes of one fixed-size C value (SQL_C_SLONG -> 4, SQL_C_NUMERIC -> sizeof(SQL_NUMERIC_STRUCT) ...);
/// 0 for SQL_C_CHAR/WCHAR/BINARY/DEFAULT whose element size is the bound buffer length (api_results.cpp).
size_t cTypeSize(SQLSMALLINT cType);

// ---- Shared implementations (called by both ANSI and Unicode entry points) -------------------
namespace impl {
SQLRETURN connect(Connection& c, std::optional<std::string> dsn, std::optional<std::string> uid, std::optional<std::string> pwd);
SQLRETURN driverConnect(Connection& c, SQLHWND hwnd, std::string inConn, TextOut& outConn, SQLUSMALLINT completion);
SQLRETURN browseConnect(Connection& c, std::string inConn, TextOut& outConn);
SQLRETURN prepare(Statement& s, std::string sql);
SQLRETURN execDirect(Statement& s, std::string sql);
SQLRETURN nativeSql(Connection& c, std::string in, TextOut& out);
SQLRETURN getInfo(Connection& c, SQLUSMALLINT infoType, SQLPOINTER value, SQLSMALLINT bufLen, SQLSMALLINT* strLen, bool wide);
SQLRETURN getDiagRec(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT rec, TextOut& sqlstate, SQLINTEGER* native, TextOut& message);
SQLRETURN getDiagField(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLSMALLINT bufLen, SQLSMALLINT* strLen, bool wide);
SQLRETURN describeCol(Statement& s, SQLUSMALLINT col, TextOut& name, SQLSMALLINT* dataType, SQLULEN* columnSize, SQLSMALLINT* decimalDigits, SQLSMALLINT* nullable);
SQLRETURN colAttribute(Statement& s, SQLUSMALLINT col, SQLUSMALLINT field, SQLPOINTER charAttr, SQLSMALLINT bufLen, SQLSMALLINT* strLen, SQLLEN* numAttr, bool wide);
SQLRETURN getCursorName(Statement& s, TextOut& name);
SQLRETURN setCursorName(Statement& s, std::string name);
SQLRETURN tables(Statement& s, CatalogArgs args);
SQLRETURN columns(Statement& s, CatalogArgs args);
SQLRETURN primaryKeys(Statement& s, CatalogArgs args);
SQLRETURN foreignKeys(Statement& s, CatalogArgs pk, CatalogArgs fk);
SQLRETURN statistics(Statement& s, CatalogArgs args, SQLUSMALLINT unique, SQLUSMALLINT reserved);
SQLRETURN specialColumns(Statement& s, SQLUSMALLINT idType, CatalogArgs args, SQLUSMALLINT scope, SQLUSMALLINT nullable);
SQLRETURN procedures(Statement& s, CatalogArgs args);
SQLRETURN procedureColumns(Statement& s, CatalogArgs args);
SQLRETURN tablePrivileges(Statement& s, CatalogArgs args);
SQLRETURN columnPrivileges(Statement& s, CatalogArgs args);
SQLRETURN getTypeInfo(Statement& s, SQLSMALLINT dataType);
SQLRETURN getConnectAttr(Connection& c, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide);
SQLRETURN setConnectAttr(Connection& c, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER strLen, bool wide);
SQLRETURN getStmtAttr(Statement& s, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide);
SQLRETURN setStmtAttr(Statement& s, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER strLen, bool wide);
SQLRETURN getDescField(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide);
SQLRETURN setDescField(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER bufLen, bool wide);
SQLRETURN getDescRec(Descriptor& d, SQLSMALLINT rec, TextOut& name, SQLSMALLINT* type, SQLSMALLINT* subType, SQLLEN* length, SQLSMALLINT* precision, SQLSMALLINT* scale, SQLSMALLINT* nullable);
SQLRETURN setDescRec(Descriptor& d, SQLSMALLINT rec, SQLSMALLINT type, SQLSMALLINT subType, SQLLEN length, SQLSMALLINT precision, SQLSMALLINT scale, SQLPOINTER data, SQLLEN* stringLength, SQLLEN* indicator);
/// Installs a MemoryRowSource as the statement's result (used by every catalog function).
SQLRETURN installCatalogResult(Statement& s, std::unique_ptr<MemoryRowSource> rows, const char* dynamicFunction);
/// Core of SQLExecute / SQLExecDirect once `s.sql` and parameters are in place (api_statement.cpp).
SQLRETURN executeStatement(Statement& s);
/// Core of SQLFetch / SQLFetchScroll(SQL_FETCH_NEXT) (api_results.cpp).
SQLRETURN fetchRowset(Statement& s);
/// Lazily runs DESCRIBE OUTPUT / DESCRIBE INPUT for a prepared statement (api_statement.cpp).
void ensureDescribedOutput(Statement& s);
void ensureDescribedInput(Statement& s);
}  // namespace impl

}  // namespace trino_odbc
