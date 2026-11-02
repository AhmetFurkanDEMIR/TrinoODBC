// trino_odbc_bench: throughput benchmark for the Trino ODBC driver.
//
//   trino_odbc_bench <driver path or DSN-less connection string> [--sql "<query>"] [--rows N] [--wide]
//                    [--array-size N] [--getdata]
//
// Loads the driver directly (no driver manager needed), executes the query with a block cursor and
// bound columns, and reports rows/s and bytes/s. Every column is bound to its default C type
// (SQL_C_WCHAR for character data unless --narrow) so the numbers reflect what Power BI sees.
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <sqlucode.h>

#ifndef SQL_OV_ODBC3_80
#  define SQL_OV_ODBC3_80 380UL
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

struct Api {
  SQLRETURN(SQL_API* AllocHandle)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
  SQLRETURN(SQL_API* FreeHandle)(SQLSMALLINT, SQLHANDLE);
  SQLRETURN(SQL_API* SetEnvAttr)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
  SQLRETURN(SQL_API* DriverConnect)(SQLHDBC, SQLHWND, SQLCHAR*, SQLSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLUSMALLINT);
  SQLRETURN(SQL_API* Disconnect)(SQLHDBC);
  SQLRETURN(SQL_API* ExecDirect)(SQLHSTMT, SQLCHAR*, SQLINTEGER);
  SQLRETURN(SQL_API* NumResultCols)(SQLHSTMT, SQLSMALLINT*);
  SQLRETURN(SQL_API* DescribeCol)(SQLHSTMT, SQLUSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*, SQLULEN*, SQLSMALLINT*, SQLSMALLINT*);
  SQLRETURN(SQL_API* BindCol)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*);
  SQLRETURN(SQL_API* SetStmtAttr)(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER);
  SQLRETURN(SQL_API* Fetch)(SQLHSTMT);
  SQLRETURN(SQL_API* GetData)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*);
  SQLRETURN(SQL_API* GetDiagRec)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR*, SQLINTEGER*, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*);
  SQLRETURN(SQL_API* FreeStmt)(SQLHSTMT, SQLUSMALLINT);
};

void* loadSym(void* lib, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
  return dlsym(lib, name);
#endif
}

void printDiag(const Api& api, SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[6], msg[1024];
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  for (SQLSMALLINT i = 1; api.GetDiagRec(type, h, i, state, &native, msg, sizeof msg, &len) == SQL_SUCCESS; ++i)
    std::fprintf(stderr, "  %s (%d) %s\n", state, static_cast<int>(native), msg);
}

bool ok(SQLRETURN rc) { return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; }

struct Bound {
  SQLSMALLINT cType;
  size_t elemSize;
  std::vector<unsigned char> data;
  std::vector<SQLLEN> ind;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s \"Driver=...;Host=...;...\" [--sql q] [--array-size N] [--narrow] [--getdata]\n", argv[0]);
    return 2;
  }
  std::string connStr = argv[1];
  std::string sql = "SELECT * FROM tpch.sf1.orders";
  SQLULEN arraySize = 1000;
  bool narrow = false, useGetData = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--sql" && i + 1 < argc) sql = argv[++i];
    else if (a == "--array-size" && i + 1 < argc) arraySize = static_cast<SQLULEN>(std::atol(argv[++i]));
    else if (a == "--narrow") narrow = true;
    else if (a == "--getdata") useGetData = true;
  }
  // The driver path is the Driver= key of the connection string.
  std::string driverPath;
  {
    size_t p = connStr.find("Driver=");
    if (p == std::string::npos) p = connStr.find("DRIVER=");
    if (p != std::string::npos) {
      size_t e = connStr.find(';', p);
      driverPath = connStr.substr(p + 7, e == std::string::npos ? std::string::npos : e - p - 7);
    }
  }
  if (driverPath.empty()) {
    std::fprintf(stderr, "connection string must contain Driver=<path to driver library>\n");
    return 2;
  }
#if defined(_WIN32)
  void* lib = LoadLibraryA(driverPath.c_str());
#else
  void* lib = dlopen(driverPath.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  if (!lib) {
    std::fprintf(stderr, "cannot load %s\n", driverPath.c_str());
    return 2;
  }
  Api api{};
#define LOAD(field, name) api.field = reinterpret_cast<decltype(api.field)>(loadSym(lib, name)); if (!api.field) { std::fprintf(stderr, "missing %s\n", name); return 2; }
  LOAD(AllocHandle, "SQLAllocHandle"); LOAD(FreeHandle, "SQLFreeHandle"); LOAD(SetEnvAttr, "SQLSetEnvAttr");
  LOAD(DriverConnect, "SQLDriverConnect"); LOAD(Disconnect, "SQLDisconnect"); LOAD(ExecDirect, "SQLExecDirect");
  LOAD(NumResultCols, "SQLNumResultCols"); LOAD(DescribeCol, "SQLDescribeCol"); LOAD(BindCol, "SQLBindCol");
  LOAD(SetStmtAttr, "SQLSetStmtAttr"); LOAD(Fetch, "SQLFetch"); LOAD(GetData, "SQLGetData");
  LOAD(GetDiagRec, "SQLGetDiagRec"); LOAD(FreeStmt, "SQLFreeStmt");
#undef LOAD

  SQLHENV env = nullptr; SQLHDBC dbc = nullptr; SQLHSTMT stmt = nullptr;
  api.AllocHandle(SQL_HANDLE_ENV, nullptr, &env);
  api.SetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80), 0);
  api.AllocHandle(SQL_HANDLE_DBC, env, &dbc);
  SQLCHAR out[2048]; SQLSMALLINT outLen = 0;
  const auto t0 = std::chrono::steady_clock::now();
  if (!ok(api.DriverConnect(dbc, nullptr, reinterpret_cast<SQLCHAR*>(connStr.data()), SQL_NTS, out, sizeof out, &outLen, SQL_DRIVER_NOPROMPT))) {
    std::fprintf(stderr, "connect failed\n"); printDiag(api, SQL_HANDLE_DBC, dbc); return 1;
  }
  const auto t1 = std::chrono::steady_clock::now();
  api.AllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
  if (!ok(api.ExecDirect(stmt, reinterpret_cast<SQLCHAR*>(sql.data()), SQL_NTS))) {
    std::fprintf(stderr, "execute failed\n"); printDiag(api, SQL_HANDLE_STMT, stmt); return 1;
  }
  const auto t2 = std::chrono::steady_clock::now();
  SQLSMALLINT ncols = 0;
  api.NumResultCols(stmt, &ncols);
  std::vector<Bound> cols(static_cast<size_t>(ncols));
  size_t rowBytes = 0;
  for (SQLSMALLINT c = 1; c <= ncols; ++c) {
    SQLCHAR name[256]; SQLSMALLINT nameLen, type, digits, nullable; SQLULEN size = 0;
    api.DescribeCol(stmt, static_cast<SQLUSMALLINT>(c), name, sizeof name, &nameLen, &type, &size, &digits, &nullable);
    Bound& b = cols[static_cast<size_t>(c - 1)];
    switch (type) {
      case SQL_BIGINT: case SQL_INTEGER: case SQL_SMALLINT: case SQL_TINYINT: b.cType = SQL_C_SBIGINT; b.elemSize = 8; break;
      case SQL_DOUBLE: case SQL_REAL: case SQL_FLOAT: b.cType = SQL_C_DOUBLE; b.elemSize = 8; break;
      case SQL_BIT: b.cType = SQL_C_BIT; b.elemSize = 1; break;
      case SQL_TYPE_DATE: b.cType = SQL_C_TYPE_DATE; b.elemSize = sizeof(SQL_DATE_STRUCT); break;
      case SQL_TYPE_TIMESTAMP: b.cType = SQL_C_TYPE_TIMESTAMP; b.elemSize = sizeof(SQL_TIMESTAMP_STRUCT); break;
      default: {
        size_t chars = size == 0 || size > 4000 ? 4000 : static_cast<size_t>(size);
        if (narrow) { b.cType = SQL_C_CHAR; b.elemSize = chars * 4 + 1; }
        else { b.cType = SQL_C_WCHAR; b.elemSize = (chars + 1) * sizeof(SQLWCHAR); }
        break;
      }
    }
    rowBytes += b.elemSize;
    b.data.resize(b.elemSize * arraySize);
    b.ind.resize(arraySize);
    std::printf("col %d %-24s type=%d size=%lu -> C type %d\n", c, name, type, static_cast<unsigned long>(size), b.cType);
    if (!useGetData) api.BindCol(stmt, static_cast<SQLUSMALLINT>(c), b.cType, b.data.data(), static_cast<SQLLEN>(b.elemSize), b.ind.data());
  }
  SQLULEN fetched = 0;
  std::vector<SQLUSMALLINT> status(arraySize);
  api.SetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(useGetData ? 1 : arraySize), 0);
  api.SetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0);
  api.SetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, status.data(), 0);
  unsigned long long rows = 0, bytes = 0, warnings = 0;
  std::vector<unsigned char> scratch(4096 * 4 + 16);
  for (;;) {
    SQLRETURN rc = api.Fetch(stmt);
    if (rc == SQL_NO_DATA) break;
    if (!ok(rc)) { std::fprintf(stderr, "fetch failed after %llu rows\n", rows); printDiag(api, SQL_HANDLE_STMT, stmt); return 1; }
    if (rc == SQL_SUCCESS_WITH_INFO) ++warnings;
    if (useGetData) {
      for (SQLSMALLINT c = 1; c <= ncols; ++c) {
        SQLLEN ind = 0;
        api.GetData(stmt, static_cast<SQLUSMALLINT>(c), cols[static_cast<size_t>(c - 1)].cType, scratch.data(), static_cast<SQLLEN>(scratch.size()), &ind);
        if (ind > 0) bytes += static_cast<unsigned long long>(ind);
      }
      rows += 1;
    } else {
      rows += fetched;
      for (auto& b : cols)
        for (SQLULEN i = 0; i < fetched; ++i)
          if (b.ind[i] > 0) bytes += static_cast<unsigned long long>(b.ind[i]);
    }
  }
  const auto t3 = std::chrono::steady_clock::now();
  using ms = std::chrono::duration<double, std::milli>;
  const double connectMs = ms(t1 - t0).count(), execMs = ms(t2 - t1).count(), fetchMs = ms(t3 - t2).count();
  std::printf("rows=%llu columns=%d payload=%.1f MB warnings=%llu\n", rows, ncols, bytes / 1048576.0, warnings);
  std::printf("connect %.0f ms | execute (first page) %.0f ms | fetch %.0f ms | %.0f rows/s | %.1f MB/s\n",
              connectMs, execMs, fetchMs, rows / (fetchMs / 1000.0), bytes / 1048576.0 / (fetchMs / 1000.0));
  api.FreeStmt(stmt, SQL_CLOSE);
  api.FreeHandle(SQL_HANDLE_STMT, stmt);
  api.Disconnect(dbc);
  api.FreeHandle(SQL_HANDLE_DBC, dbc);
  api.FreeHandle(SQL_HANDLE_ENV, env);
  return 0;
}
