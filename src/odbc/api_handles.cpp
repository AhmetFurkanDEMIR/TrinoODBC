// Handle allocation and release: SQLAllocHandle / SQLFreeHandle and their ODBC 2 wrappers.
// See ODBC-API-DETAILS.md "api_handles.cpp" and ARCHITECTURE.md §4.
#include "odbc/api_common.h"
#include "odbc/stmt_results.h"

#include "client/trino_client.h"
#include "util/errors.h"

#include <mutex>
#include <new>
#include <vector>

namespace trino_odbc {

// Owned by api_common.cpp: drops the SQLError walk position of a diagnostic area.
namespace diag_cursor {
void reset(const Diag& d);
}

namespace {

constexpr const char* kLog = "odbc";

/// Environment allocation has no handle to report on, so failures can only surface as SQL_ERROR.
SQLRETURN allocEnv(SQLHANDLE* out) {
  if (!out) return SQL_ERROR;
  *out = SQL_NULL_HENV;
  try {
    // Process-wide one-time setup lives here because this is the first driver call every
    // application makes.
    Logger::global().configureFromEnvironment();
    TrinoClient::globalInit();
    *out = allocEnvironment();
    TRINO_LOG_TRACE(kLog, "SQLAllocHandle(ENV) -> 0");
    return SQL_SUCCESS;
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "SQLAllocHandle(ENV) failed: %s", e.what());
    return SQL_ERROR;
  } catch (...) {
    TRINO_LOG_ERROR(kLog, "SQLAllocHandle(ENV) failed: unknown exception");
    return SQL_ERROR;
  }
}

SQLRETURN allocDbc(SQLHANDLE env, SQLHANDLE* out) {
  if (out) *out = SQL_NULL_HDBC;
  return guardEnv(env, "SQLAllocHandle(DBC)", [&](Environment& e) -> SQLRETURN {
    if (!out) throw DriverError(sqlstate::kInvalidNullPointer, "OutputHandlePtr is a null pointer");
    if (e.odbcVersion == 0) {
      throw DriverError(sqlstate::kFunctionSequence,
                        "SQL_ATTR_ODBC_VERSION must be set with SQLSetEnvAttr before allocating a connection");
    }
    Connection* c = e.createConnection();
    *out = c;
    return SQL_SUCCESS;
  });
}

SQLRETURN allocStmt(SQLHANDLE dbc, SQLHANDLE* out) {
  if (out) *out = SQL_NULL_HSTMT;
  return guardDbc(dbc, "SQLAllocHandle(STMT)", [&](Connection& c) -> SQLRETURN {
    if (!out) throw DriverError(sqlstate::kInvalidNullPointer, "OutputHandlePtr is a null pointer");
    requireConnected(c);
    Statement* s = c.createStatement();
    // Connection-level defaults that seed statement attributes.
    s->queryTimeout = c.config.queryTimeoutSec > 0 ? static_cast<SQLULEN>(c.config.queryTimeoutSec) : 0;
    s->metadataId = c.metadataId;
    *out = s;
    return SQL_SUCCESS;
  });
}

SQLRETURN allocDesc(SQLHANDLE dbc, SQLHANDLE* out) {
  if (out) *out = SQL_NULL_HDESC;
  return guardDbc(dbc, "SQLAllocHandle(DESC)", [&](Connection& c) -> SQLRETURN {
    if (!out) throw DriverError(sqlstate::kInvalidNullPointer, "OutputHandlePtr is a null pointer");
    requireConnected(c);
    Descriptor* d = c.createDescriptor();
    *out = d;
    return SQL_SUCCESS;
  });
}

/// HY092 for an unknown HandleType is reported on whichever valid handle the caller passed.
SQLRETURN badHandleType(SQLHANDLE input, SQLSMALLINT type) {
  auto fail = [type]() -> SQLRETURN {
    throw DriverError(sqlstate::kInvalidAttribute,
                      "Invalid handle type " + std::to_string(type) + " passed to SQLAllocHandle");
  };
  if (asEnvironment(input)) return guardEnv(input, "SQLAllocHandle", [&](Environment&) { return fail(); });
  if (asConnection(input)) return guardDbc(input, "SQLAllocHandle", [&](Connection&) { return fail(); });
  if (asStatement(input)) return guardStmt(input, "SQLAllocHandle", [&](Statement&) { return fail(); });
  if (asDescriptor(input)) return guardDesc(input, "SQLAllocHandle", [&](Descriptor&) { return fail(); });
  return SQL_INVALID_HANDLE;
}

SQLRETURN freeEnv(SQLHANDLE h) {
  Environment* env = asEnvironment(h);
  if (!env) return SQL_INVALID_HANDLE;
  {
    // The check runs under the lock; the lock must be gone before the object is deleted.
    std::unique_lock<std::recursive_mutex> lock(env->mutex);
    env->diag.clear();
    diag_cursor::reset(env->diag);
    if (!env->connections.empty()) {
      const SQLRETURN rc = env->diag.addError(
          sqlstate::kFunctionSequence,
          "Function sequence error: the environment still has " + std::to_string(env->connections.size()) +
              " allocated connection handle(s); free them first");
      env->diag.returnCode = rc;
      TRINO_LOG_TRACE(kLog, "SQLFreeHandle(ENV) -> %d", static_cast<int>(rc));
      return rc;
    }
  }
  freeEnvironment(env);
  TRINO_LOG_TRACE(kLog, "SQLFreeHandle(ENV) -> 0");
  return SQL_SUCCESS;
}

SQLRETURN freeDbc(SQLHANDLE h) {
  Connection* c = asConnection(h);
  if (!c) return SQL_INVALID_HANDLE;
  Environment* env = c->env;
  {
    std::unique_lock<std::recursive_mutex> lock(c->mutex);
    c->diag.clear();
    diag_cursor::reset(c->diag);
    if (c->connected) {
      const SQLRETURN rc = c->diag.addError(sqlstate::kFunctionSequence,
                                            "Function sequence error: the connection is still open; call SQLDisconnect first");
      c->diag.returnCode = rc;
      TRINO_LOG_TRACE(kLog, "SQLFreeHandle(DBC) -> %d", static_cast<int>(rc));
      return rc;
    }
    // Statements and explicit descriptors are released by SQLDisconnect; anything still attached
    // (direct dlopen use without a driver manager) is cleaned up by destroyConnection below.
    for (Statement* s : c->statements) diag_cursor::reset(s->diag);
    for (Descriptor* d : c->descriptors) diag_cursor::reset(d->diag);
  }
  if (env) {
    // Environment before connection: the same order SQLEndTran(ENV) and SQLFreeHandle(ENV) use.
    HandleLock envLock(env->mutex);
    env->destroyConnection(c);
  } else {
    // Cannot happen for handles created through SQLAllocHandle; kept so the handle never leaks.
    Environment orphan;
    orphan.destroyConnection(c);
  }
  TRINO_LOG_TRACE(kLog, "SQLFreeHandle(DBC) -> 0");
  return SQL_SUCCESS;
}

SQLRETURN freeStmt(SQLHANDLE h) {
  // Cancel-before-lock ordering lives in impl::freeStatementHandle (stmt_results.h) so that a
  // statement blocked in SQLFetch/SQLExecute is unblocked instead of waited for.
  return impl::freeStatementHandle(static_cast<SQLHSTMT>(h));
}

SQLRETURN freeDesc(SQLHANDLE h) {
  Descriptor* d = asDescriptor(h);
  if (!d) return SQL_INVALID_HANDLE;
  Connection* c = d->conn;
  if (!c) return SQL_INVALID_HANDLE;
  HandleLock connLock(c->mutex);
  if (!asDescriptor(h)) return SQL_INVALID_HANDLE;
  d->diag.clear();
  diag_cursor::reset(d->diag);
  if (!d->isExplicit()) {
    // Implicit descriptors live and die with their statement.
    const SQLRETURN rc = d->diag.addError(sqlstate::kInvalidUseOfDescriptor,
                                          "Invalid use of an automatically allocated descriptor handle: "
                                          "implicit descriptors are freed with their statement");
    d->diag.returnCode = rc;
    TRINO_LOG_TRACE(kLog, "SQLFreeHandle(DESC) -> %d", static_cast<int>(rc));
    return rc;
  }
  // destroyDescriptor reverts every statement using this descriptor to its implicit one.
  c->destroyDescriptor(d);
  TRINO_LOG_TRACE(kLog, "SQLFreeHandle(DESC) -> 0");
  return SQL_SUCCESS;
}

}  // namespace
}  // namespace trino_odbc

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle, SQLHANDLE* OutputHandle) {
  switch (HandleType) {
    case SQL_HANDLE_ENV:
      return allocEnv(OutputHandle);
    case SQL_HANDLE_DBC:
      return allocDbc(InputHandle, OutputHandle);
    case SQL_HANDLE_STMT:
      return allocStmt(InputHandle, OutputHandle);
    case SQL_HANDLE_DESC:
      return allocDesc(InputHandle, OutputHandle);
    default:
      if (OutputHandle) *OutputHandle = SQL_NULL_HANDLE;
      return badHandleType(InputHandle, HandleType);
  }
}

TRINO_ODBC_API SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle) {
  switch (HandleType) {
    case SQL_HANDLE_ENV:
      return freeEnv(Handle);
    case SQL_HANDLE_DBC:
      return freeDbc(Handle);
    case SQL_HANDLE_STMT:
      return freeStmt(Handle);
    case SQL_HANDLE_DESC:
      return freeDesc(Handle);
    default:
      return SQL_INVALID_HANDLE;
  }
}

TRINO_ODBC_API SQLRETURN SQL_API SQLAllocEnv(SQLHENV* EnvironmentHandle) {
  const SQLRETURN rc = allocEnv(EnvironmentHandle);
  if (rc == SQL_SUCCESS && EnvironmentHandle && *EnvironmentHandle) {
    // An application using the ODBC 2 allocation call never sets SQL_ATTR_ODBC_VERSION itself
    // (the driver manager does it on its behalf when one is present).
    if (Environment* env = asEnvironment(*EnvironmentHandle)) {
      HandleLock lock(env->mutex);
      if (env->odbcVersion == 0) env->odbcVersion = SQL_OV_ODBC2;
    }
  }
  return rc;
}

TRINO_ODBC_API SQLRETURN SQL_API SQLAllocConnect(SQLHENV EnvironmentHandle, SQLHDBC* ConnectionHandle) {
  return allocDbc(EnvironmentHandle, ConnectionHandle);
}

TRINO_ODBC_API SQLRETURN SQL_API SQLAllocStmt(SQLHDBC ConnectionHandle, SQLHSTMT* StatementHandle) {
  return allocStmt(ConnectionHandle, StatementHandle);
}

TRINO_ODBC_API SQLRETURN SQL_API SQLFreeEnv(SQLHENV EnvironmentHandle) { return freeEnv(EnvironmentHandle); }

TRINO_ODBC_API SQLRETURN SQL_API SQLFreeConnect(SQLHDBC ConnectionHandle) { return freeDbc(ConnectionHandle); }

}  // extern "C"
