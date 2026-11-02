#pragma once
// Integration-test harness for the Trino ODBC driver.
//
// The tests load the driver shared library directly (dlopen / LoadLibrary) and call its exported
// SQL* entry points through the `Odbc` function-pointer table below, so no driver manager is
// involved and every ODBC 3.8 export can be checked. Only the public ODBC headers are used here:
// the harness must never see driver internals, otherwise it would stop testing the ABI.
//
// Everything wide-string related goes through fromWide()/toWide(): SQLWCHAR is 2 bytes on
// Windows / unixODBC and 4 bytes on iODBC, and the same test sources must work on all three.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#ifndef ODBCVER
#  define ODBCVER 0x0380
#endif
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <sqlucode.h>

#include "unit/test_framework.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef SQL_API
#  define SQL_API
#endif
// ODBC 3.8 constants that older driver-manager headers (iODBC) do not define.
#ifndef SQL_OV_ODBC3_80
#  define SQL_OV_ODBC3_80 380UL
#endif
#ifndef SQL_ATTR_METADATA_ID
#  define SQL_ATTR_METADATA_ID 10014
#endif
#ifndef SQL_API_SQLCANCELHANDLE
#  define SQL_API_SQLCANCELHANDLE 1550
#endif
#ifndef SQL_ATTR_ASYNC_DBC_FUNCTIONS
#  define SQL_ATTR_ASYNC_DBC_FUNCTIONS 117
#endif
#ifndef SQL_ASYNC_DBC_FUNCTIONS
#  define SQL_ASYNC_DBC_FUNCTIONS 10023
#endif
#ifndef SQL_DRIVER_AWARE_POOLING_SUPPORTED
#  define SQL_DRIVER_AWARE_POOLING_SUPPORTED 10024
#endif
#ifndef SQL_C_GUID
#  define SQL_C_GUID SQL_GUID
#endif
#ifndef SQL_NO_ROW_NUMBER
#  define SQL_NO_ROW_NUMBER (-1)
#endif
#ifndef SQL_NO_COLUMN_NUMBER
#  define SQL_NO_COLUMN_NUMBER (-1)
#endif

namespace harness {

// ================================================================================================
// Exported entry points (ANSI + Unicode + ODBC 2 compatibility). One X-macro row per export so the
// same list drives the typedefs, the struct members and the symbol resolution / completeness check.
// Prototypes follow the ODBC 3.8 64-bit ABI (SQLLEN / SQLULEN where the spec says so).
// ================================================================================================
// clang-format off
#define HARNESS_ODBC_FUNCTIONS(X) \
  X(SQLAllocHandle,       (SQLSMALLINT, SQLHANDLE, SQLHANDLE*)) \
  X(SQLFreeHandle,        (SQLSMALLINT, SQLHANDLE)) \
  X(SQLAllocEnv,          (SQLHENV*)) \
  X(SQLAllocConnect,      (SQLHENV, SQLHDBC*)) \
  X(SQLAllocStmt,         (SQLHDBC, SQLHSTMT*)) \
  X(SQLFreeEnv,           (SQLHENV)) \
  X(SQLFreeConnect,       (SQLHDBC)) \
  X(SQLConnect,           (SQLHDBC, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLDriverConnect,     (SQLHDBC, SQLHWND, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLUSMALLINT)) \
  X(SQLBrowseConnect,     (SQLHDBC, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLDisconnect,        (SQLHDBC)) \
  X(SQLEndTran,           (SQLSMALLINT, SQLHANDLE, SQLSMALLINT)) \
  X(SQLTransact,          (SQLHENV, SQLHDBC, SQLUSMALLINT)) \
  X(SQLGetInfo,           (SQLHDBC, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetFunctions,      (SQLHDBC, SQLUSMALLINT, SQLUSMALLINT*)) \
  X(SQLGetTypeInfo,       (SQLHSTMT, SQLSMALLINT)) \
  X(SQLSetEnvAttr,        (SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetEnvAttr,        (SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetConnectAttr,    (SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetConnectAttr,    (SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetConnectOption,  (SQLHDBC, SQLUSMALLINT, SQLULEN)) \
  X(SQLGetConnectOption,  (SQLHDBC, SQLUSMALLINT, SQLPOINTER)) \
  X(SQLSetStmtAttr,       (SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetStmtAttr,       (SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetStmtOption,     (SQLHSTMT, SQLUSMALLINT, SQLULEN)) \
  X(SQLGetStmtOption,     (SQLHSTMT, SQLUSMALLINT, SQLPOINTER)) \
  X(SQLPrepare,           (SQLHSTMT, SQLCHAR*, SQLINTEGER)) \
  X(SQLExecute,           (SQLHSTMT)) \
  X(SQLExecDirect,        (SQLHSTMT, SQLCHAR*, SQLINTEGER)) \
  X(SQLNativeSql,         (SQLHDBC, SQLCHAR*, SQLINTEGER, SQLCHAR*, SQLINTEGER, SQLINTEGER*)) \
  X(SQLNumParams,         (SQLHSTMT, SQLSMALLINT*)) \
  X(SQLDescribeParam,     (SQLHSTMT, SQLUSMALLINT, SQLSMALLINT*, SQLULEN*, SQLSMALLINT*, SQLSMALLINT*)) \
  X(SQLBindParameter,     (SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLSMALLINT, SQLSMALLINT, SQLULEN, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*)) \
  X(SQLSetParam,          (SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLSMALLINT, SQLULEN, SQLSMALLINT, SQLPOINTER, SQLLEN*)) \
  X(SQLParamOptions,      (SQLHSTMT, SQLULEN, SQLULEN*)) \
  X(SQLParamData,         (SQLHSTMT, SQLPOINTER*)) \
  X(SQLPutData,           (SQLHSTMT, SQLPOINTER, SQLLEN)) \
  X(SQLCancel,            (SQLHSTMT)) \
  X(SQLCancelHandle,      (SQLSMALLINT, SQLHANDLE)) \
  X(SQLRowCount,          (SQLHSTMT, SQLLEN*)) \
  X(SQLMoreResults,       (SQLHSTMT)) \
  X(SQLCloseCursor,       (SQLHSTMT)) \
  X(SQLFreeStmt,          (SQLHSTMT, SQLUSMALLINT)) \
  X(SQLGetCursorName,     (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLSetCursorName,     (SQLHSTMT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLSetPos,            (SQLHSTMT, SQLSETPOSIROW, SQLUSMALLINT, SQLUSMALLINT)) \
  X(SQLBulkOperations,    (SQLHSTMT, SQLSMALLINT)) \
  X(SQLBindCol,           (SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*)) \
  X(SQLFetch,             (SQLHSTMT)) \
  X(SQLFetchScroll,       (SQLHSTMT, SQLSMALLINT, SQLLEN)) \
  X(SQLExtendedFetch,     (SQLHSTMT, SQLUSMALLINT, SQLLEN, SQLULEN*, SQLUSMALLINT*)) \
  X(SQLGetData,           (SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*)) \
  X(SQLNumResultCols,     (SQLHSTMT, SQLSMALLINT*)) \
  X(SQLDescribeCol,       (SQLHSTMT, SQLUSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*, SQLULEN*, SQLSMALLINT*, SQLSMALLINT*)) \
  X(SQLColAttribute,      (SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*, SQLLEN*)) \
  X(SQLColAttributes,     (SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*, SQLLEN*)) \
  X(SQLTables,            (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLColumns,           (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLPrimaryKeys,       (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLForeignKeys,       (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLStatistics,        (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT)) \
  X(SQLSpecialColumns,    (SQLHSTMT, SQLUSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT)) \
  X(SQLProcedures,        (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLProcedureColumns,  (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLTablePrivileges,   (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLColumnPrivileges,  (SQLHSTMT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT)) \
  X(SQLGetDiagRec,        (SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR*, SQLINTEGER*, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetDiagField,      (SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLError,             (SQLHENV, SQLHDBC, SQLHSTMT, SQLCHAR*, SQLINTEGER*, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLCopyDesc,          (SQLHDESC, SQLHDESC)) \
  X(SQLGetDescField,      (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetDescField,      (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetDescRec,        (SQLHDESC, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*, SQLSMALLINT*, SQLLEN*, SQLSMALLINT*, SQLSMALLINT*, SQLSMALLINT*)) \
  X(SQLSetDescRec,        (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLSMALLINT, SQLLEN, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN*, SQLLEN*)) \
  X(SQLSetScrollOptions,  (SQLHSTMT, SQLUSMALLINT, SQLLEN, SQLUSMALLINT)) \
  /* --- Unicode --- */ \
  X(SQLConnectW,          (SQLHDBC, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLDriverConnectW,    (SQLHDBC, SQLHWND, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLUSMALLINT)) \
  X(SQLBrowseConnectW,    (SQLHDBC, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetInfoW,          (SQLHDBC, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetTypeInfoW,      (SQLHSTMT, SQLSMALLINT)) \
  X(SQLSetConnectAttrW,   (SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetConnectAttrW,   (SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetConnectOptionW, (SQLHDBC, SQLUSMALLINT, SQLULEN)) \
  X(SQLGetConnectOptionW, (SQLHDBC, SQLUSMALLINT, SQLPOINTER)) \
  X(SQLSetStmtAttrW,      (SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetStmtAttrW,      (SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLPrepareW,          (SQLHSTMT, SQLWCHAR*, SQLINTEGER)) \
  X(SQLExecDirectW,       (SQLHSTMT, SQLWCHAR*, SQLINTEGER)) \
  X(SQLNativeSqlW,        (SQLHDBC, SQLWCHAR*, SQLINTEGER, SQLWCHAR*, SQLINTEGER, SQLINTEGER*)) \
  X(SQLGetCursorNameW,    (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLSetCursorNameW,    (SQLHSTMT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLDescribeColW,      (SQLHSTMT, SQLUSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*, SQLULEN*, SQLSMALLINT*, SQLSMALLINT*)) \
  X(SQLColAttributeW,     (SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*, SQLLEN*)) \
  X(SQLColAttributesW,    (SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*, SQLLEN*)) \
  X(SQLTablesW,           (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLColumnsW,          (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLPrimaryKeysW,      (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLForeignKeysW,      (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLStatisticsW,       (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT)) \
  X(SQLSpecialColumnsW,   (SQLHSTMT, SQLUSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT)) \
  X(SQLProceduresW,       (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLProcedureColumnsW, (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLTablePrivilegesW,  (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLColumnPrivilegesW, (SQLHSTMT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT)) \
  X(SQLGetDiagRecW,       (SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLWCHAR*, SQLINTEGER*, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetDiagFieldW,     (SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLErrorW,            (SQLHENV, SQLHDBC, SQLHSTMT, SQLWCHAR*, SQLINTEGER*, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*)) \
  X(SQLGetDescFieldW,     (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLINTEGER, SQLINTEGER*)) \
  X(SQLSetDescFieldW,     (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLINTEGER)) \
  X(SQLGetDescRecW,       (SQLHDESC, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*, SQLSMALLINT*, SQLLEN*, SQLSMALLINT*, SQLSMALLINT*, SQLSMALLINT*)) \
  X(SQLSetDescRecW,       (SQLHDESC, SQLSMALLINT, SQLSMALLINT, SQLSMALLINT, SQLLEN, SQLSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN*, SQLLEN*))
// clang-format on

struct OdbcApi {
#define HARNESS_DECLARE_MEMBER(name, args) \
  using name##_t = SQLRETURN(SQL_API*) args;   \
  name##_t name = nullptr;
  HARNESS_ODBC_FUNCTIONS(HARNESS_DECLARE_MEMBER)
#undef HARNESS_DECLARE_MEMBER
};

/// The loaded driver. Populated by loadDriver(); null members are exports the driver lacks.
extern OdbcApi Odbc;

/// Loads the driver library and resolves every export. Throws std::runtime_error when the library
/// cannot be loaded or a function the harness itself needs is missing. Names of other missing
/// exports are returned so a test can report them.
std::vector<std::string> loadDriver(const std::string& path);
/// Path the driver was loaded from ("" before loadDriver).
const std::string& driverPath();
/// Exports that loadDriver() could not resolve (empty when the driver is complete).
const std::vector<std::string>& missingExports();

// ================================================================================================
// Wide-string helpers (SQLWCHAR may be UTF-16 or UTF-32; the tests never assume either)
// ================================================================================================
constexpr bool kWideIsUtf16 = sizeof(SQLWCHAR) == 2;

/// UTF-8 -> SQLWCHAR units, NUL-terminated (the terminator is included in the vector).
std::vector<SQLWCHAR> toWide(std::string_view utf8);
/// SQLWCHAR units -> UTF-8. `units` < 0 means NUL-terminated.
std::string fromWide(const SQLWCHAR* text, SQLLEN units = -1);
/// Number of SQLWCHAR units in a UTF-8 string (without terminator).
SQLLEN wideLength(std::string_view utf8);

/// Owns a wide buffer for passing text into a *W entry point.
struct WStr {
  std::vector<SQLWCHAR> buf;
  explicit WStr(std::string_view utf8) : buf(toWide(utf8)) {}
  SQLWCHAR* ptr() { return buf.data(); }
  SQLSMALLINT len() const { return static_cast<SQLSMALLINT>(buf.size() - 1); }  // units without NUL
  SQLINTEGER ilen() const { return static_cast<SQLINTEGER>(buf.size() - 1); }
};

/// Narrow text (UTF-8) for the ANSI entry points.
inline SQLCHAR* narrow(const std::string& s) { return reinterpret_cast<SQLCHAR*>(const_cast<char*>(s.c_str())); }
inline SQLCHAR* narrow(const char* s) { return reinterpret_cast<SQLCHAR*>(const_cast<char*>(s)); }

// ================================================================================================
// Diagnostics and check macros
// ================================================================================================
struct DiagRec {
  std::string sqlstate;
  SQLINTEGER native = 0;
  std::string message;
};

/// Every diagnostic record of a handle (SQLGetDiagRecW when exported, else SQLGetDiagRec).
std::vector<DiagRec> diag(SQLSMALLINT handleType, SQLHANDLE handle);
/// One-line rendering of diag() for failure messages.
std::string diagText(SQLSMALLINT handleType, SQLHANDLE handle);
/// True when a record with that SQLSTATE exists.
bool hasState(const std::vector<DiagRec>& records, const char* sqlstate);
bool hasState(SQLSMALLINT handleType, SQLHANDLE handle, const char* sqlstate);
/// First record's SQLSTATE ("" when none).
std::string firstState(SQLSMALLINT handleType, SQLHANDLE handle);
const char* rcName(SQLRETURN rc);

/// Reports a failed return code with the handle's diagnostics. Returns SQL_SUCCEEDED(rc).
bool checkRc(SQLRETURN rc, SQLSMALLINT handleType, SQLHANDLE handle, const char* what, const char* file, int line,
             bool fatal);
/// Reports when `rc` is not SQL_ERROR or the diag lacks `sqlstate`. Returns true when both hold.
bool checkError(SQLRETURN rc, SQLSMALLINT handleType, SQLHANDLE handle, const char* sqlstate, const char* what,
                const char* file, int line);

#define ODBC_CHECK(rc, type, h) ::harness::checkRc((rc), (type), (h), #rc, __FILE__, __LINE__, false)
#define ODBC_REQUIRE(rc, type, h) ::harness::checkRc((rc), (type), (h), #rc, __FILE__, __LINE__, true)
#define ODBC_CHECK_ERROR(rc, type, h, state) ::harness::checkError((rc), (type), (h), (state), #rc, __FILE__, __LINE__)
#define ODBC_CHECK_STATE(type, h, state)                                                                   \
  do {                                                                                                     \
    ::testfw::State::get().checks++;                                                                       \
    if (!::harness::hasState((type), (h), (state)))                                                        \
      ::testfw::fail(__FILE__, __LINE__, std::string("expected SQLSTATE ") + (state) + ", diag: " +      \
                                             ::harness::diagText((type), (h)), false);                    \
  } while (0)

/// Marks the current test as skipped (prints why) — the caller must `return` afterwards.
void skip(const std::string& why);
int skippedCount();
/// Informational line in the test output (timings, rows/sec ...).
void info(const char* fmt, ...);

// ================================================================================================
// Test context (filled by main.cpp)
// ================================================================================================
struct Context {
  std::string connStr;         // base connection string (mock or --connstr)
  bool mock = false;           // true when the harness launched the mock servers
  std::string host = "127.0.0.1";
  int port = 0;                // plain mock port (0 when not mock)
  int basicAuthPort = 0;       // mock with --auth basic:alice:secret (0 when unavailable)
  int bearerPort = 0;          // mock with --auth bearer:<bearerToken>
  std::string bearerToken = "mock-bearer-token-123";
  int tlsPort = 0;             // mock with --tls (0 when the cert could not be generated)
  std::string tlsCertFile;     // PEM of the TLS mock
  int logLevel = 0;
  std::string logPath;
  /// Connection string for the given mock port with extra keys appended.
  std::string mockConnStr(int port, const std::string& extra = "") const;
  /// Base connection string plus extra keys ("Key=Value;...").
  std::string connStrWith(const std::string& extra) const;
};
Context& ctx();

/// True when the connected server is the mock (harness flag or SQL_DBMS_VER containing "mock").
bool serverIsMock(SQLHDBC dbc);
#define HARNESS_MOCK_ONLY()                                        \
  do {                                                             \
    if (!::harness::ctx().mock) {                                  \
      ::harness::skip("requires the mock server");                 \
      return;                                                      \
    }                                                              \
  } while (0)

// ================================================================================================
// RAII handles
// ================================================================================================
class Env {
 public:
  Env();  // SQL_ATTR_ODBC_VERSION = SQL_OV_ODBC3_80
  explicit Env(SQLINTEGER odbcVersion);
  ~Env();
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  SQLHENV h = SQL_NULL_HENV;
};

class Dbc {
 public:
  explicit Dbc(Env& env);
  ~Dbc();
  Dbc(const Dbc&) = delete;
  Dbc& operator=(const Dbc&) = delete;
  /// SQLDriverConnectW with SQL_DRIVER_NOPROMPT. Returns the rc; `outConnStr` receives the out string.
  SQLRETURN connect(const std::string& connStr, std::string* outConnStr = nullptr);
  /// ANSI variant.
  SQLRETURN connectA(const std::string& connStr, std::string* outConnStr = nullptr);
  SQLRETURN disconnect();
  bool connected() const { return connected_; }
  SQLHDBC h = SQL_NULL_HDBC;

 private:
  bool connected_ = false;
};

class Stmt {
 public:
  explicit Stmt(Dbc& dbc);
  explicit Stmt(SQLHDBC dbc);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  SQLHSTMT h = SQL_NULL_HSTMT;
};

/// Env + connected Dbc using the context connection string (REQUIREs the connect to succeed).
class Session {
 public:
  explicit Session(const std::string& extraKeys = "");
  Env env;
  Dbc dbc;
  SQLHDBC h() { return dbc.h; }
};

// ================================================================================================
// Data helpers (SQLGetData based; chunk loops handle 01004 so long values come back whole)
// ================================================================================================
SQLRETURN execDirect(SQLHSTMT stmt, const std::string& sql);   // SQLExecDirectW
SQLRETURN execDirectA(SQLHSTMT stmt, const std::string& sql);  // SQLExecDirect
SQLRETURN prepare(SQLHSTMT stmt, const std::string& sql);      // SQLPrepareW

/// Column value as UTF-8 via SQL_C_CHAR. `isNull` (optional) receives the NULL flag.
std::string getString(SQLHSTMT stmt, SQLUSMALLINT col, bool* isNull = nullptr, SQLLEN chunkBytes = 256);
/// Column value via SQL_C_WCHAR, transcoded to UTF-8.
std::string getWString(SQLHSTMT stmt, SQLUSMALLINT col, bool* isNull = nullptr, SQLLEN chunkBytes = 256);
std::optional<int64_t> getInt64(SQLHSTMT stmt, SQLUSMALLINT col);
std::optional<double> getDouble(SQLHSTMT stmt, SQLUSMALLINT col);
std::optional<std::vector<unsigned char>> getBinary(SQLHSTMT stmt, SQLUSMALLINT col);
SQLSMALLINT numResultCols(SQLHSTMT stmt);
/// Column names via SQLDescribeColW.
std::vector<std::string> columnNames(SQLHSTMT stmt);
/// SQLDescribeColW of one column (REQUIREs success).
struct ColumnDesc {
  std::string name;
  SQLSMALLINT type = 0;
  SQLULEN size = 0;
  SQLSMALLINT digits = 0;
  SQLSMALLINT nullable = 0;
};
ColumnDesc describeCol(SQLHSTMT stmt, SQLUSMALLINT col);
/// SQLColAttributeW character attribute (UTF-8). `rcOut` receives the return code when given.
std::string colAttrString(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT field, SQLRETURN* rcOut = nullptr);
/// SQLColAttributeW numeric attribute.
SQLLEN colAttrNum(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT field, SQLRETURN* rcOut = nullptr);
/// Integer -> SQLPOINTER for the attribute setters.
inline SQLPOINTER attrVal(uintptr_t v) { return reinterpret_cast<SQLPOINTER>(v); }
/// SQLGetStmtAttr / SQLGetConnectAttr integer value (0 on failure; `rcOut` receives the rc).
SQLULEN getStmtAttrInt(SQLHSTMT stmt, SQLINTEGER attr, SQLRETURN* rcOut = nullptr);
SQLULEN getConnectAttrInt(SQLHDBC dbc, SQLINTEGER attr, SQLRETURN* rcOut = nullptr);
/// Fetches every remaining row, each cell as text ("<NULL>" for NULL).
std::vector<std::vector<std::string>> fetchAll(SQLHSTMT stmt);
/// Number of remaining rows (fetch loop without reading data).
int64_t countRows(SQLHSTMT stmt);
/// SQLGetInfoW string value.
std::string getInfoString(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut = nullptr);
SQLUINTEGER getInfoUInt(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut = nullptr);
SQLUSMALLINT getInfoUSmall(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut = nullptr);

// ================================================================================================
// Networking / process helpers used by main.cpp and the mock-stats assertions
// ================================================================================================
/// Minimal HTTP/1.0 GET (no TLS). Returns the body, nullopt on connection failure or non-2xx.
std::optional<std::string> httpGet(const std::string& host, int port, const std::string& path, int timeoutMs = 5000);
/// Minimal HTTP/1.0 POST with an empty body.
std::optional<std::string> httpPost(const std::string& host, int port, const std::string& path, int timeoutMs = 5000);
/// Integer member of a flat JSON object (`"key": 123`).
std::optional<int64_t> jsonInt(const std::string& json, const char* key);

struct MockStats {
  int64_t queries = 0, cancelled = 0, acks = 0, requests = 0, segmentsServed = 0, errors = 0;
};
/// GET /mock/stats of the plain mock (nullopt when not running in mock mode or unreachable).
std::optional<MockStats> mockStats();
bool mockReset();

/// Picks a free TCP port on 127.0.0.1 (bind to port 0).
int findFreePort();
/// Waits until a TCP connect to host:port succeeds.
bool waitForPort(const std::string& host, int port, int timeoutMs);
/// Returns true when a TCP connect succeeds right now.
bool portOpen(const std::string& host, int port, int timeoutMs = 500);

/// A child process (the mock server). Killed in the destructor.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  /// Starts `argv[0]` (searched in PATH) with the given arguments. Returns false on failure.
  bool start(const std::vector<std::string>& argv, std::string* error);
  bool running() const;
  void kill();
  bool started() const { return started_; }

 private:
  bool started_ = false;
#ifdef _WIN32
  void* process_ = nullptr;
#else
  long pid_ = 0;
#endif
};

double nowSeconds();
void sleepMs(int ms);
std::string envOr(const char* name, const std::string& fallback);

}  // namespace harness
