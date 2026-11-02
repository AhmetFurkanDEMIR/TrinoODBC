// Integration-test harness implementation: driver loading, wide-string transcoding, diagnostics,
// RAII handles, SQLGetData helpers, a minimal socket HTTP client (for /mock/stats) and the child
// process wrapper that main.cpp uses to run the mock server.
#include "integration/odbc_loader.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
#else
#  include <arpa/inet.h>
#  include <dlfcn.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace harness {

OdbcApi Odbc;

namespace {

std::string g_driverPath;
std::vector<std::string> g_missing;
int g_skipped = 0;

#ifdef _WIN32
using LibHandle = HMODULE;
LibHandle openLibrary(const std::string& path, std::string* error) {
  LibHandle h = LoadLibraryA(path.c_str());
  if (!h) *error = "LoadLibrary failed with error " + std::to_string(GetLastError());
  return h;
}
void* resolve(LibHandle h, const char* name) { return reinterpret_cast<void*>(GetProcAddress(h, name)); }
#else
using LibHandle = void*;
LibHandle openLibrary(const std::string& path, std::string* error) {
  LibHandle h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* msg = dlerror();
    *error = msg ? msg : "dlopen failed";
  }
  return h;
}
void* resolve(LibHandle h, const char* name) { return dlsym(h, name); }
#endif

}  // namespace

std::vector<std::string> loadDriver(const std::string& path) {
  std::string error;
  LibHandle lib = openLibrary(path, &error);
  if (!lib) throw std::runtime_error("cannot load driver '" + path + "': " + error);
  g_driverPath = path;
  g_missing.clear();
#define HARNESS_RESOLVE(name, args)                                                     \
  Odbc.name = reinterpret_cast<OdbcApi::name##_t>(resolve(lib, #name));                 \
  if (!Odbc.name) g_missing.push_back(#name);
  HARNESS_ODBC_FUNCTIONS(HARNESS_RESOLVE)
#undef HARNESS_RESOLVE
  // Without these the harness itself cannot run a single test.
  const char* essential[] = {"SQLAllocHandle", "SQLFreeHandle", "SQLSetEnvAttr", "SQLDriverConnectW",
                             "SQLDisconnect", "SQLGetDiagRecW", "SQLExecDirectW", "SQLFetch", "SQLGetData"};
  for (const char* name : essential) {
    if (std::find(g_missing.begin(), g_missing.end(), name) != g_missing.end()) {
      throw std::runtime_error(std::string("driver '") + path + "' does not export " + name);
    }
  }
  return g_missing;
}

const std::string& driverPath() { return g_driverPath; }
const std::vector<std::string>& missingExports() { return g_missing; }

// ================================================================================================
// Wide strings
// ================================================================================================

std::vector<SQLWCHAR> toWide(std::string_view utf8) {
  std::vector<SQLWCHAR> out;
  out.reserve(utf8.size() + 1);
  size_t i = 0;
  while (i < utf8.size()) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
      len = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
      cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
      len = 4;
    } else {
      cp = 0xFFFD;  // malformed byte: replacement character keeps the length bookkeeping sane
    }
    i += len;
    if (kWideIsUtf16 && cp > 0xFFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<SQLWCHAR>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<SQLWCHAR>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<SQLWCHAR>(cp));
    }
  }
  out.push_back(0);
  return out;
}

std::string fromWide(const SQLWCHAR* text, SQLLEN units) {
  std::string out;
  if (!text) return out;
  size_t n = 0;
  if (units < 0) {
    while (text[n] != 0) ++n;
  } else {
    n = static_cast<size_t>(units);
  }
  out.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = static_cast<uint32_t>(text[i]);
    if (kWideIsUtf16 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
      const uint32_t lo = static_cast<uint32_t>(text[i + 1]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

SQLLEN wideLength(std::string_view utf8) { return static_cast<SQLLEN>(toWide(utf8).size() - 1); }

// ================================================================================================
// Diagnostics
// ================================================================================================

std::vector<DiagRec> diag(SQLSMALLINT handleType, SQLHANDLE handle) {
  std::vector<DiagRec> out;
  for (SQLSMALLINT rec = 1; rec < 100; ++rec) {
    DiagRec r;
    SQLSMALLINT len = 0;
    SQLRETURN rc;
    if (Odbc.SQLGetDiagRecW) {
      SQLWCHAR state[8] = {0};
      std::vector<SQLWCHAR> msg(4096);
      rc = Odbc.SQLGetDiagRecW(handleType, handle, rec, state, &r.native, msg.data(),
                               static_cast<SQLSMALLINT>(msg.size()), &len);
      if (!SQL_SUCCEEDED(rc)) break;
      r.sqlstate = fromWide(state, 5);
      r.message = fromWide(msg.data(), std::min<SQLLEN>(len, static_cast<SQLLEN>(msg.size() - 1)));
    } else {
      SQLCHAR state[8] = {0};
      std::vector<SQLCHAR> msg(4096);
      rc = Odbc.SQLGetDiagRec(handleType, handle, rec, state, &r.native, msg.data(),
                              static_cast<SQLSMALLINT>(msg.size()), &len);
      if (!SQL_SUCCEEDED(rc)) break;
      r.sqlstate.assign(reinterpret_cast<const char*>(state), 5);
      r.message.assign(reinterpret_cast<const char*>(msg.data()),
                       static_cast<size_t>(std::min<SQLLEN>(len, static_cast<SQLLEN>(msg.size() - 1))));
    }
    out.push_back(std::move(r));
  }
  return out;
}

std::string diagText(SQLSMALLINT handleType, SQLHANDLE handle) {
  std::string out;
  for (const DiagRec& r : diag(handleType, handle)) {
    if (!out.empty()) out += " | ";
    out += r.sqlstate + " (" + std::to_string(r.native) + ") " + r.message;
  }
  return out.empty() ? "<no diagnostics>" : out;
}

bool hasState(const std::vector<DiagRec>& records, const char* sqlstate) {
  for (const DiagRec& r : records) {
    if (r.sqlstate == sqlstate) return true;
  }
  return false;
}

bool hasState(SQLSMALLINT handleType, SQLHANDLE handle, const char* sqlstate) {
  return hasState(diag(handleType, handle), sqlstate);
}

std::string firstState(SQLSMALLINT handleType, SQLHANDLE handle) {
  const std::vector<DiagRec> d = diag(handleType, handle);
  return d.empty() ? std::string() : d.front().sqlstate;
}

const char* rcName(SQLRETURN rc) {
  switch (rc) {
    case SQL_SUCCESS: return "SQL_SUCCESS";
    case SQL_SUCCESS_WITH_INFO: return "SQL_SUCCESS_WITH_INFO";
    case SQL_ERROR: return "SQL_ERROR";
    case SQL_INVALID_HANDLE: return "SQL_INVALID_HANDLE";
    case SQL_NO_DATA: return "SQL_NO_DATA";
    case SQL_NEED_DATA: return "SQL_NEED_DATA";
    case SQL_STILL_EXECUTING: return "SQL_STILL_EXECUTING";
    default: return "<unknown rc>";
  }
}

bool checkRc(SQLRETURN rc, SQLSMALLINT handleType, SQLHANDLE handle, const char* what, const char* file, int line,
             bool fatal) {
  testfw::State::get().checks++;
  if (SQL_SUCCEEDED(rc)) return true;
  std::string msg = std::string(what) + " returned " + rcName(rc);
  if (rc != SQL_INVALID_HANDLE && handle) msg += ": " + diagText(handleType, handle);
  testfw::fail(file, line, msg, fatal);
  return false;
}

bool checkError(SQLRETURN rc, SQLSMALLINT handleType, SQLHANDLE handle, const char* sqlstate, const char* what,
                const char* file, int line) {
  testfw::State::get().checks++;
  if (rc != SQL_ERROR) {
    testfw::fail(file, line, std::string(what) + " returned " + rcName(rc) + " but SQL_ERROR with " + sqlstate +
                                 " was expected; diag: " + diagText(handleType, handle), false);
    return false;
  }
  if (!hasState(handleType, handle, sqlstate)) {
    testfw::fail(file, line, std::string(what) + ": expected SQLSTATE " + sqlstate + ", diag: " +
                                 diagText(handleType, handle), false);
    return false;
  }
  return true;
}

void skip(const std::string& why) {
  ++g_skipped;
  std::printf("  SKIP [%s] %s\n", testfw::State::get().current.c_str(), why.c_str());
  std::fflush(stdout);
}

int skippedCount() { return g_skipped; }

void info(const char* fmt, ...) {
  std::printf("  INFO [%s] ", testfw::State::get().current.c_str());
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
  std::fflush(stdout);
}

// ================================================================================================
// Context
// ================================================================================================

Context& ctx() {
  static Context c;
  return c;
}

std::string Context::mockConnStr(int mockPort, const std::string& extra) const {
  // ODBC: the first occurrence of a keyword wins, so test-supplied keys go first and override the
  // defaults (e.g. UID=alice for the authenticated mock instances).
  std::string s = "Driver=" + g_driverPath + ";";
  if (!extra.empty()) s += extra + ";";
  s += "Host=" + host + ";Port=" + std::to_string(mockPort) + ";UID=test;Catalog=mock;Schema=demo;LogLevel=" +
       std::to_string(logLevel);
  if (!logPath.empty()) s += ";LogPath=" + logPath;
  return s;
}

std::string Context::connStrWith(const std::string& extra) const {
  if (extra.empty()) return connStr;
  std::string s = connStr;
  if (!s.empty() && s.back() != ';') s += ';';
  return s + extra;
}

bool serverIsMock(SQLHDBC dbc) {
  if (ctx().mock) return true;
  const std::string ver = getInfoString(dbc, SQL_DBMS_VER);
  return ver.find("mock") != std::string::npos;
}

// ================================================================================================
// RAII handles
// ================================================================================================

Env::Env() : Env(static_cast<SQLINTEGER>(SQL_OV_ODBC3_80)) {}

Env::Env(SQLINTEGER odbcVersion) {
  const SQLRETURN rc = Odbc.SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &h);
  if (!SQL_SUCCEEDED(rc)) throw testfw::Failure("SQLAllocHandle(ENV) failed");
  const SQLRETURN rc2 = Odbc.SQLSetEnvAttr(h, SQL_ATTR_ODBC_VERSION,
                                           reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(odbcVersion)), 0);
  if (!SQL_SUCCEEDED(rc2)) throw testfw::Failure("SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION) failed: " + diagText(SQL_HANDLE_ENV, h));
}

Env::~Env() {
  if (h != SQL_NULL_HENV) Odbc.SQLFreeHandle(SQL_HANDLE_ENV, h);
}

Dbc::Dbc(Env& env) {
  const SQLRETURN rc = Odbc.SQLAllocHandle(SQL_HANDLE_DBC, env.h, &h);
  if (!SQL_SUCCEEDED(rc)) throw testfw::Failure("SQLAllocHandle(DBC) failed: " + diagText(SQL_HANDLE_ENV, env.h));
}

Dbc::~Dbc() {
  if (h == SQL_NULL_HDBC) return;
  if (connected_) Odbc.SQLDisconnect(h);
  Odbc.SQLFreeHandle(SQL_HANDLE_DBC, h);
}

SQLRETURN Dbc::connect(const std::string& connStr, std::string* outConnStr) {
  WStr in(connStr);
  std::vector<SQLWCHAR> out(4096);
  SQLSMALLINT outLen = 0;
  const SQLRETURN rc = Odbc.SQLDriverConnectW(h, nullptr, in.ptr(), in.len(), out.data(),
                                              static_cast<SQLSMALLINT>(out.size()), &outLen, SQL_DRIVER_NOPROMPT);
  if (SQL_SUCCEEDED(rc)) connected_ = true;
  if (outConnStr) *outConnStr = SQL_SUCCEEDED(rc) ? fromWide(out.data(), std::min<SQLLEN>(outLen, 4095)) : std::string();
  return rc;
}

SQLRETURN Dbc::connectA(const std::string& connStr, std::string* outConnStr) {
  std::vector<SQLCHAR> out(4096);
  SQLSMALLINT outLen = 0;
  const SQLRETURN rc = Odbc.SQLDriverConnect(h, nullptr, narrow(connStr), static_cast<SQLSMALLINT>(connStr.size()),
                                             out.data(), static_cast<SQLSMALLINT>(out.size()), &outLen,
                                             SQL_DRIVER_NOPROMPT);
  if (SQL_SUCCEEDED(rc)) connected_ = true;
  if (outConnStr) {
    *outConnStr = SQL_SUCCEEDED(rc) ? std::string(reinterpret_cast<const char*>(out.data()),
                                                  static_cast<size_t>(std::min<SQLSMALLINT>(outLen, 4095)))
                                    : std::string();
  }
  return rc;
}

SQLRETURN Dbc::disconnect() {
  const SQLRETURN rc = Odbc.SQLDisconnect(h);
  if (SQL_SUCCEEDED(rc)) connected_ = false;
  return rc;
}

Stmt::Stmt(Dbc& dbc) : Stmt(dbc.h) {}

Stmt::Stmt(SQLHDBC dbc) {
  const SQLRETURN rc = Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h);
  if (!SQL_SUCCEEDED(rc)) throw testfw::Failure("SQLAllocHandle(STMT) failed: " + diagText(SQL_HANDLE_DBC, dbc));
}

Stmt::~Stmt() {
  if (h != SQL_NULL_HSTMT) Odbc.SQLFreeHandle(SQL_HANDLE_STMT, h);
}

Session::Session(const std::string& extraKeys) : env(), dbc(env) {
  const std::string cs = ctx().connStrWith(extraKeys);
  const SQLRETURN rc = dbc.connect(cs);
  if (!SQL_SUCCEEDED(rc)) {
    throw testfw::Failure("connect failed (" + std::string(rcName(rc)) + "): " + diagText(SQL_HANDLE_DBC, dbc.h));
  }
}

// ================================================================================================
// Data helpers
// ================================================================================================

SQLRETURN execDirect(SQLHSTMT stmt, const std::string& sql) {
  WStr w(sql);
  return Odbc.SQLExecDirectW(stmt, w.ptr(), w.ilen());
}

SQLRETURN execDirectA(SQLHSTMT stmt, const std::string& sql) {
  return Odbc.SQLExecDirect(stmt, narrow(sql), static_cast<SQLINTEGER>(sql.size()));
}

SQLRETURN prepare(SQLHSTMT stmt, const std::string& sql) {
  WStr w(sql);
  return Odbc.SQLPrepareW(stmt, w.ptr(), w.ilen());
}

std::string getString(SQLHSTMT stmt, SQLUSMALLINT col, bool* isNull, SQLLEN chunkBytes) {
  std::string out;
  if (isNull) *isNull = false;
  if (chunkBytes < 2) chunkBytes = 2;
  std::vector<char> buf(static_cast<size_t>(chunkBytes));
  for (;;) {
    SQLLEN ind = 0;
    const SQLRETURN rc = Odbc.SQLGetData(stmt, col, SQL_C_CHAR, buf.data(), chunkBytes, &ind);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLGetData(SQL_C_CHAR) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    if (ind == SQL_NULL_DATA) {
      if (isNull) *isNull = true;
      break;
    }
    const bool truncated = rc == SQL_SUCCESS_WITH_INFO && hasState(SQL_HANDLE_STMT, stmt, "01004");
    size_t copied;
    if (truncated || ind == SQL_NO_TOTAL || ind >= chunkBytes) {
      // The driver may deliver fewer bytes than the buffer holds (it avoids splitting a UTF-8
      // sequence when it can); the terminator marks the end of this chunk.
      copied = strnlen(buf.data(), static_cast<size_t>(chunkBytes));
    } else {
      copied = static_cast<size_t>(ind);
    }
    out.append(buf.data(), copied);
    if (!truncated) break;
  }
  return out;
}

std::string getWString(SQLHSTMT stmt, SQLUSMALLINT col, bool* isNull, SQLLEN chunkBytes) {
  std::vector<SQLWCHAR> units;
  if (isNull) *isNull = false;
  const SQLLEN w = static_cast<SQLLEN>(sizeof(SQLWCHAR));
  if (chunkBytes < 2 * w) chunkBytes = 2 * w;
  chunkBytes -= chunkBytes % w;
  std::vector<SQLWCHAR> buf(static_cast<size_t>(chunkBytes / w));
  for (;;) {
    SQLLEN ind = 0;
    const SQLRETURN rc = Odbc.SQLGetData(stmt, col, SQL_C_WCHAR, buf.data(), chunkBytes, &ind);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLGetData(SQL_C_WCHAR) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    if (ind == SQL_NULL_DATA) {
      if (isNull) *isNull = true;
      break;
    }
    const bool truncated = rc == SQL_SUCCESS_WITH_INFO && hasState(SQL_HANDLE_STMT, stmt, "01004");
    size_t copiedUnits;
    if (truncated || ind == SQL_NO_TOTAL || ind >= chunkBytes) {
      copiedUnits = 0;
      while (copiedUnits < buf.size() && buf[copiedUnits] != 0) ++copiedUnits;  // up to the terminator
    } else {
      copiedUnits = static_cast<size_t>(ind / w);
    }
    units.insert(units.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(copiedUnits));
    if (!truncated) break;
  }
  return fromWide(units.data(), static_cast<SQLLEN>(units.size()));
}

std::optional<int64_t> getInt64(SQLHSTMT stmt, SQLUSMALLINT col) {
  SQLBIGINT v = 0;
  SQLLEN ind = 0;
  const SQLRETURN rc = Odbc.SQLGetData(stmt, col, SQL_C_SBIGINT, &v, sizeof(v), &ind);
  if (!SQL_SUCCEEDED(rc)) {
    testfw::fail(__FILE__, __LINE__, "SQLGetData(SQL_C_SBIGINT) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return static_cast<int64_t>(v);
}

std::optional<double> getDouble(SQLHSTMT stmt, SQLUSMALLINT col) {
  double v = 0;
  SQLLEN ind = 0;
  const SQLRETURN rc = Odbc.SQLGetData(stmt, col, SQL_C_DOUBLE, &v, sizeof(v), &ind);
  if (!SQL_SUCCEEDED(rc)) {
    testfw::fail(__FILE__, __LINE__, "SQLGetData(SQL_C_DOUBLE) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
  }
  if (ind == SQL_NULL_DATA) return std::nullopt;
  return v;
}

std::optional<std::vector<unsigned char>> getBinary(SQLHSTMT stmt, SQLUSMALLINT col) {
  std::vector<unsigned char> out;
  unsigned char buf[64];
  for (;;) {
    SQLLEN ind = 0;
    const SQLRETURN rc = Odbc.SQLGetData(stmt, col, SQL_C_BINARY, buf, sizeof(buf), &ind);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLGetData(SQL_C_BINARY) failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    if (ind == SQL_NULL_DATA) return std::nullopt;
    const bool truncated = rc == SQL_SUCCESS_WITH_INFO && hasState(SQL_HANDLE_STMT, stmt, "01004");
    const size_t copied = (truncated || ind == SQL_NO_TOTAL || ind >= static_cast<SQLLEN>(sizeof(buf)))
                              ? sizeof(buf)
                              : static_cast<size_t>(ind);
    out.insert(out.end(), buf, buf + copied);
    if (!truncated) break;
  }
  return out;
}

SQLSMALLINT numResultCols(SQLHSTMT stmt) {
  SQLSMALLINT n = 0;
  const SQLRETURN rc = Odbc.SQLNumResultCols(stmt, &n);
  if (!SQL_SUCCEEDED(rc)) {
    testfw::fail(__FILE__, __LINE__, "SQLNumResultCols failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
  }
  return n;
}

std::vector<std::string> columnNames(SQLHSTMT stmt) {
  std::vector<std::string> names;
  const SQLSMALLINT n = numResultCols(stmt);
  for (SQLSMALLINT i = 1; i <= n; ++i) {
    std::vector<SQLWCHAR> name(512);
    SQLSMALLINT len = 0, type = 0, digits = 0, nullable = 0;
    SQLULEN size = 0;
    const SQLRETURN rc = Odbc.SQLDescribeColW(stmt, static_cast<SQLUSMALLINT>(i), name.data(),
                                              static_cast<SQLSMALLINT>(name.size()), &len, &type, &size, &digits,
                                              &nullable);
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLDescribeColW failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    names.push_back(fromWide(name.data(), std::min<SQLLEN>(len, 511)));
  }
  return names;
}

ColumnDesc describeCol(SQLHSTMT stmt, SQLUSMALLINT col) {
  ColumnDesc d;
  std::vector<SQLWCHAR> name(512);
  SQLSMALLINT len = 0;
  const SQLRETURN rc = Odbc.SQLDescribeColW(stmt, col, name.data(), static_cast<SQLSMALLINT>(name.size()), &len,
                                            &d.type, &d.size, &d.digits, &d.nullable);
  if (!SQL_SUCCEEDED(rc)) {
    testfw::fail(__FILE__, __LINE__, "SQLDescribeColW(" + std::to_string(col) + ") failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
  }
  d.name = fromWide(name.data(), std::min<SQLLEN>(len, 511));
  return d;
}

std::string colAttrString(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT field, SQLRETURN* rcOut) {
  std::vector<SQLWCHAR> buf(1024);
  SQLSMALLINT lenBytes = 0;
  SQLLEN num = 0;
  const SQLRETURN rc = Odbc.SQLColAttributeW(stmt, col, field, buf.data(),
                                             static_cast<SQLSMALLINT>(buf.size() * sizeof(SQLWCHAR)), &lenBytes, &num);
  if (rcOut) *rcOut = rc;
  if (!SQL_SUCCEEDED(rc)) return std::string();
  const SQLLEN units = std::min<SQLLEN>(lenBytes / static_cast<SQLSMALLINT>(sizeof(SQLWCHAR)),
                                        static_cast<SQLLEN>(buf.size() - 1));
  return fromWide(buf.data(), units);
}

SQLLEN colAttrNum(SQLHSTMT stmt, SQLUSMALLINT col, SQLUSMALLINT field, SQLRETURN* rcOut) {
  SQLLEN num = 0;
  SQLSMALLINT len = 0;
  const SQLRETURN rc = Odbc.SQLColAttributeW(stmt, col, field, nullptr, 0, &len, &num);
  if (rcOut) *rcOut = rc;
  return num;
}

SQLULEN getStmtAttrInt(SQLHSTMT stmt, SQLINTEGER attr, SQLRETURN* rcOut) {
  SQLULEN v = 0;
  SQLINTEGER len = 0;
  const SQLRETURN rc = Odbc.SQLGetStmtAttrW(stmt, attr, &v, SQL_IS_UINTEGER, &len);
  if (rcOut) *rcOut = rc;
  return v;
}

SQLULEN getConnectAttrInt(SQLHDBC dbc, SQLINTEGER attr, SQLRETURN* rcOut) {
  SQLULEN v = 0;
  SQLINTEGER len = 0;
  const SQLRETURN rc = Odbc.SQLGetConnectAttrW(dbc, attr, &v, SQL_IS_UINTEGER, &len);
  if (rcOut) *rcOut = rc;
  return v;
}

std::vector<std::vector<std::string>> fetchAll(SQLHSTMT stmt) {
  std::vector<std::vector<std::string>> rows;
  const SQLSMALLINT n = numResultCols(stmt);
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetch(stmt);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLFetch failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    std::vector<std::string> row;
    row.reserve(static_cast<size_t>(n));
    for (SQLSMALLINT c = 1; c <= n; ++c) {
      bool isNull = false;
      std::string v = getString(stmt, static_cast<SQLUSMALLINT>(c), &isNull, 1024);
      row.push_back(isNull ? std::string("<NULL>") : std::move(v));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

int64_t countRows(SQLHSTMT stmt) {
  int64_t n = 0;
  for (;;) {
    const SQLRETURN rc = Odbc.SQLFetch(stmt);
    if (rc == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, "SQLFetch failed: " + diagText(SQL_HANDLE_STMT, stmt), true);
    }
    ++n;
  }
  return n;
}

std::string getInfoString(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut) {
  std::vector<SQLWCHAR> buf(2048);
  SQLSMALLINT lenBytes = 0;
  const SQLRETURN rc = Odbc.SQLGetInfoW(dbc, infoType, buf.data(),
                                        static_cast<SQLSMALLINT>(buf.size() * sizeof(SQLWCHAR)), &lenBytes);
  if (rcOut) *rcOut = rc;
  if (!SQL_SUCCEEDED(rc)) return std::string();
  const SQLLEN units = std::min<SQLLEN>(lenBytes / static_cast<SQLSMALLINT>(sizeof(SQLWCHAR)),
                                        static_cast<SQLLEN>(buf.size() - 1));
  return fromWide(buf.data(), units);
}

SQLUINTEGER getInfoUInt(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut) {
  SQLUINTEGER v = 0;
  SQLSMALLINT len = 0;
  const SQLRETURN rc = Odbc.SQLGetInfoW(dbc, infoType, &v, sizeof(v), &len);
  if (rcOut) *rcOut = rc;
  return v;
}

SQLUSMALLINT getInfoUSmall(SQLHDBC dbc, SQLUSMALLINT infoType, SQLRETURN* rcOut) {
  SQLUSMALLINT v = 0;
  SQLSMALLINT len = 0;
  const SQLRETURN rc = Odbc.SQLGetInfoW(dbc, infoType, &v, sizeof(v), &len);
  if (rcOut) *rcOut = rc;
  return v;
}

// ================================================================================================
// Sockets
// ================================================================================================

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
bool wouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
void ensureSocketsInit() {
  static bool done = false;
  if (!done) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    done = true;
  }
}
void setNonBlocking(SocketHandle s, bool on) {
  u_long mode = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &mode);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) { ::close(s); }
bool wouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
void ensureSocketsInit() {
  static bool done = false;
  if (!done) {
    ::signal(SIGPIPE, SIG_IGN);  // a closed peer must not kill the test process
    done = true;
  }
}
void setNonBlocking(SocketHandle s, bool on) {
  const int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

/// Connects with a timeout; returns kInvalidSocket on failure.
SocketHandle connectTo(const std::string& host, int port, int timeoutMs) {
  ensureSocketsInit();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return kInvalidSocket;
  SocketHandle result = kInvalidSocket;
  for (addrinfo* ai = res; ai && result == kInvalidSocket; ai = ai->ai_next) {
    SocketHandle s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kInvalidSocket) continue;
    setNonBlocking(s, true);
    int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    bool ok = rc == 0;
    if (!ok && wouldBlock()) {
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(s, &wfds);
      timeval tv{};
      tv.tv_sec = timeoutMs / 1000;
      tv.tv_usec = (timeoutMs % 1000) * 1000;
      if (::select(static_cast<int>(s + 1), nullptr, &wfds, nullptr, &tv) > 0) {
        int err = 0;
#ifdef _WIN32
        int len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
        ok = err == 0;
      }
    }
    if (ok) {
      setNonBlocking(s, false);
#ifdef _WIN32
      DWORD to = static_cast<DWORD>(timeoutMs);
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
#else
      timeval to{};
      to.tv_sec = timeoutMs / 1000;
      to.tv_usec = (timeoutMs % 1000) * 1000;
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
#endif
      result = s;
    } else {
      closeSocket(s);
    }
  }
  freeaddrinfo(res);
  return result;
}

bool sendAll(SocketHandle s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const auto n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

std::optional<std::string> httpRequest(const char* method, const std::string& host, int port, const std::string& path,
                                       int timeoutMs) {
  SocketHandle s = connectTo(host, port, timeoutMs);
  if (s == kInvalidSocket) return std::nullopt;
  const std::string request = std::string(method) + " " + path + " HTTP/1.0\r\nHost: " + host + ":" +
                              std::to_string(port) + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  std::string response;
  if (sendAll(s, request)) {
    char buf[4096];
    for (;;) {
      const auto n = ::recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
    }
  }
  closeSocket(s);
  if (response.compare(0, 5, "HTTP/") != 0) return std::nullopt;
  const size_t statusPos = response.find(' ');
  if (statusPos == std::string::npos) return std::nullopt;
  const int status = std::atoi(response.c_str() + statusPos + 1);
  if (status < 200 || status >= 300) return std::nullopt;
  const size_t bodyPos = response.find("\r\n\r\n");
  return bodyPos == std::string::npos ? std::string() : response.substr(bodyPos + 4);
}

}  // namespace

std::optional<std::string> httpGet(const std::string& host, int port, const std::string& path, int timeoutMs) {
  return httpRequest("GET", host, port, path, timeoutMs);
}

std::optional<std::string> httpPost(const std::string& host, int port, const std::string& path, int timeoutMs) {
  return httpRequest("POST", host, port, path, timeoutMs);
}

std::optional<int64_t> jsonInt(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return std::nullopt;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') return std::nullopt;
  int64_t v = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    v = v * 10 + (json[pos] - '0');
    ++pos;
  }
  return negative ? -v : v;
}

std::optional<MockStats> mockStats() {
  if (!ctx().mock || ctx().port == 0) return std::nullopt;
  const std::optional<std::string> body = httpGet(ctx().host, ctx().port, "/mock/stats");
  if (!body) return std::nullopt;
  MockStats s;
  s.queries = jsonInt(*body, "queries").value_or(0);
  s.cancelled = jsonInt(*body, "cancelled").value_or(0);
  s.acks = jsonInt(*body, "acks").value_or(0);
  s.requests = jsonInt(*body, "requests").value_or(0);
  s.segmentsServed = jsonInt(*body, "segments_served").value_or(0);
  s.errors = jsonInt(*body, "errors").value_or(0);
  return s;
}

bool mockReset() {
  if (!ctx().mock || ctx().port == 0) return false;
  return httpPost(ctx().host, ctx().port, "/mock/reset").has_value();
}

int findFreePort() {
  ensureSocketsInit();
  SocketHandle s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == kInvalidSocket) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int port = 0;
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) port = ntohs(addr.sin_port);
  }
  closeSocket(s);
  return port;
}

bool portOpen(const std::string& host, int port, int timeoutMs) {
  SocketHandle s = connectTo(host, port, timeoutMs);
  if (s == kInvalidSocket) return false;
  closeSocket(s);
  return true;
}

bool waitForPort(const std::string& host, int port, int timeoutMs) {
  const double deadline = nowSeconds() + timeoutMs / 1000.0;
  while (nowSeconds() < deadline) {
    if (portOpen(host, port, 500)) return true;
    sleepMs(100);
  }
  return false;
}

// ================================================================================================
// Child process
// ================================================================================================

ChildProcess::~ChildProcess() { kill(); }

#ifdef _WIN32

namespace {
std::string quoteArg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  return out + "\"";
}
}  // namespace

bool ChildProcess::start(const std::vector<std::string>& argv, std::string* error) {
  if (argv.empty()) {
    if (error) *error = "empty argv";
    return false;
  }
  std::string cmdline;
  for (const std::string& a : argv) {
    if (!cmdline.empty()) cmdline += ' ';
    cmdline += quoteArg(a);
  }
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<char> buf(cmdline.begin(), cmdline.end());
  buf.push_back('\0');
  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    if (error) *error = "CreateProcess failed with error " + std::to_string(GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  started_ = true;
  return true;
}

bool ChildProcess::running() const {
  if (!started_ || !process_) return false;
  return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void ChildProcess::kill() {
  if (!started_ || !process_) return;
  TerminateProcess(static_cast<HANDLE>(process_), 0);
  WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
  CloseHandle(static_cast<HANDLE>(process_));
  process_ = nullptr;
  started_ = false;
}

#else

bool ChildProcess::start(const std::vector<std::string>& argv, std::string* error) {
  if (argv.empty()) {
    if (error) *error = "empty argv";
    return false;
  }
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = 0;
  // posix_spawnp instead of fork(): the harness is multi-threaded while tests run and spawning
  // from a threaded process through fork() is undefined for anything but exec.
  const int rc = posix_spawnp(&pid, argv[0].c_str(), nullptr, nullptr, args.data(), environ);
  if (rc != 0) {
    if (error) *error = std::string("posix_spawnp(") + argv[0] + ") failed: " + std::strerror(rc);
    return false;
  }
  pid_ = pid;
  started_ = true;
  return true;
}

bool ChildProcess::running() const {
  if (!started_ || pid_ <= 0) return false;
  int status = 0;
  const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return r == 0;
}

void ChildProcess::kill() {
  if (!started_ || pid_ <= 0) return;
  ::kill(static_cast<pid_t>(pid_), SIGTERM);
  int status = 0;
  for (int i = 0; i < 50; ++i) {
    if (waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) != 0) {
      pid_ = 0;
      started_ = false;
      return;
    }
    sleepMs(100);
  }
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  waitpid(static_cast<pid_t>(pid_), &status, 0);
  pid_ = 0;
  started_ = false;
}

#endif

double nowSeconds() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string envOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

}  // namespace harness
