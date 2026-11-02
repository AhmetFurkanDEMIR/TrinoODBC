// Shared plumbing for the exported ODBC entry points: text argument conversion (TextIn/TextOut),
// the guard* wrappers (handle validation, locking, diagnostics, exception mapping, tracing), state
// checks and the SQLError (ODBC 2) record cursor. See ODBC-API-DETAILS.md "api_common.cpp".
#include "odbc/api_common.h"

#include "util/errors.h"
#include "util/strings.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "odbc";

/// Adds the record for an exception that escaped an entry-point body. The order of the catch
/// clauses matters: TrinoQueryError must be recognised before its DriverError base (there is no
/// RTTI, so Diag::addException cannot tell them apart), and std::bad_alloc must map to HY001.
template <typename Body>
SQLRETURN runBody(Diag& diag, const char* fn, Body&& body) {
  try {
    return body();
  } catch (const TrinoQueryError& e) {
    TRINO_LOG_DEBUG(kLog, "%s: server error %s: %s", fn, e.errorName().c_str(), e.what());
    return diag.addServerError(e);
  } catch (const HttpError& e) {
    TRINO_LOG_DEBUG(kLog, "%s: transport error (%s, HTTP %ld): %s", fn, e.sqlstate().c_str(), e.httpStatus(), e.what());
    return diag.addException(e);
  } catch (const CancelledError& e) {
    TRINO_LOG_DEBUG(kLog, "%s: canceled: %s", fn, e.what());
    return diag.addException(e);
  } catch (const DriverError& e) {
    TRINO_LOG_DEBUG(kLog, "%s: %s %s", fn, e.sqlstate().c_str(), e.what());
    return diag.addException(e);
  } catch (const std::bad_alloc&) {
    TRINO_LOG_ERROR(kLog, "%s: out of memory", fn);
    return diag.addError(sqlstate::kMemoryAllocation, "Memory allocation error");
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "%s: unexpected exception: %s", fn, e.what());
    return diag.addException(e);
  } catch (...) {
    TRINO_LOG_ERROR(kLog, "%s: unknown exception", fn);
    return diag.addError(sqlstate::kGeneralError, "Unknown internal error");
  }
}

/// A body may add records without adjusting its return code (e.g. a warning added deep inside a
/// helper); the diagnostic area is the source of truth, so SQL_SUCCESS is upgraded accordingly.
SQLRETURN reconcile(const Diag& diag, SQLRETURN rc) {
  if (rc != SQL_SUCCESS || diag.empty()) return rc;
  for (const DiagRecord& r : diag.records()) {
    if (!r.isWarning()) return SQL_ERROR;
  }
  return SQL_SUCCESS_WITH_INFO;
}

void traceReturn(const char* fn, SQLRETURN rc, const Diag& diag) {
  if (!TRINO_LOG_ENABLED(LogLevel::Trace)) return;
  if (rc == SQL_ERROR || rc == SQL_SUCCESS_WITH_INFO) {
    const DiagRecord* r = diag.record(1);
    TRINO_LOG_TRACE(kLog, "%s -> %d (%s: %s)", fn, static_cast<int>(rc), r ? r->sqlstate.c_str() : "",
                    r ? r->message.c_str() : "");
  } else {
    TRINO_LOG_TRACE(kLog, "%s -> %d", fn, static_cast<int>(rc));
  }
}

// ---- SQLError cursor registry -------------------------------------------------------------------
// SQLError (ODBC 2) hands out the records of a diagnostic area one call at a time. The position is
// kept outside Diag (frozen header) in a map keyed by the Diag address; the guards reset it whenever
// they clear a diagnostic area so a new function call starts a new walk. The atomic counter lets
// the hot path (SQLFetch/SQLGetData) skip the map entirely until SQLError has ever been used.
std::mutex& cursorMutex() {
  static std::mutex* m = new std::mutex();  // leaked on purpose: usable during static destruction
  return *m;
}
std::map<const Diag*, size_t>& cursorMap() {
  static std::map<const Diag*, size_t>* m = new std::map<const Diag*, size_t>();
  return *m;
}
std::atomic<int> g_cursorsActive{0};

}  // namespace

namespace diag_cursor {

/// 1-based index of the next record SQLError should return for `d`, or 0 when exhausted
/// (the cursor entry is dropped so the next walk starts from the first record).
size_t take(const Diag& d) {
  std::lock_guard<std::mutex> lock(cursorMutex());
  auto& map = cursorMap();
  auto it = map.find(&d);
  size_t next = it == map.end() ? 0 : it->second;
  if (next >= d.count()) {
    if (it != map.end()) {
      map.erase(it);
      g_cursorsActive.fetch_sub(1, std::memory_order_relaxed);
    }
    return 0;
  }
  if (it == map.end()) {
    map.emplace(&d, next + 1);
    g_cursorsActive.fetch_add(1, std::memory_order_relaxed);
  } else {
    it->second = next + 1;
  }
  return next + 1;
}

/// Forgets the SQLError position of `d` (called when the area is cleared or its handle freed).
void reset(const Diag& d) {
  if (g_cursorsActive.load(std::memory_order_relaxed) == 0) return;
  std::lock_guard<std::mutex> lock(cursorMutex());
  auto& map = cursorMap();
  auto it = map.find(&d);
  if (it != map.end()) {
    map.erase(it);
    g_cursorsActive.fetch_sub(1, std::memory_order_relaxed);
  }
}

}  // namespace diag_cursor

// ---- Input text ---------------------------------------------------------------------------------

std::optional<std::string> textIn(const SQLCHAR* s, SQLLEN length, AnsiEncoding enc) {
  if (!s) return std::nullopt;
  return narrowToUtf8(s, length, enc);
}

std::optional<std::string> textIn(const SQLWCHAR* s, SQLLEN length) {
  if (!s) return std::nullopt;
  return wideToUtf8(s, length);
}

std::optional<std::string> textInBytes(const SQLPOINTER s, SQLINTEGER lengthBytes, bool wide, AnsiEncoding enc) {
  if (!s) return std::nullopt;
  if (wide) {
    const SQLLEN units = lengthBytes < 0 ? static_cast<SQLLEN>(SQL_NTS)
                                         : static_cast<SQLLEN>(lengthBytes) / static_cast<SQLLEN>(sizeof(SQLWCHAR));
    return wideToUtf8(static_cast<const SQLWCHAR*>(s), units);
  }
  return narrowToUtf8(static_cast<const SQLCHAR*>(s), static_cast<SQLLEN>(lengthBytes), enc);
}

// ---- Output text --------------------------------------------------------------------------------

TextOut::TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLSMALLINT* outLen, AnsiEncoding enc)
    : buf_(buf), bufLen_(bufLen), wide_(false), lengthInBytes_(true), enc_(enc), out16_(outLen) {}
TextOut::TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLINTEGER* outLen, AnsiEncoding enc)
    : buf_(buf), bufLen_(bufLen), wide_(false), lengthInBytes_(true), enc_(enc), out32_(outLen) {}
#if !defined(_WIN32) || defined(_WIN64)
TextOut::TextOut(SQLCHAR* buf, SQLLEN bufLen, SQLLEN* outLen, AnsiEncoding enc)
    : buf_(buf), bufLen_(bufLen), wide_(false), lengthInBytes_(true), enc_(enc), outLen_(outLen) {}
#endif
TextOut::TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLSMALLINT* outLen, bool lengthInBytes)
    : buf_(buf), bufLen_(bufLen), wide_(true), lengthInBytes_(lengthInBytes), out16_(outLen) {}
TextOut::TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLINTEGER* outLen, bool lengthInBytes)
    : buf_(buf), bufLen_(bufLen), wide_(true), lengthInBytes_(lengthInBytes), out32_(outLen) {}
#if !defined(_WIN32) || defined(_WIN64)
TextOut::TextOut(SQLWCHAR* buf, SQLLEN bufLen, SQLLEN* outLen, bool lengthInBytes)
    : buf_(buf), bufLen_(bufLen), wide_(true), lengthInBytes_(lengthInBytes), outLen_(outLen) {}
#endif
TextOut::TextOut() = default;

void TextOut::setOutLen(SQLLEN v) {
  if (out16_) *out16_ = static_cast<SQLSMALLINT>(v);
  if (out32_) *out32_ = static_cast<SQLINTEGER>(v);
  if (outLen_) *outLen_ = v;
}

bool TextOut::write(std::string_view utf8) {
  // The discarding sink still reports nothing: it represents an argument the caller did not ask for.
  if (!buf_ && !out16_ && !out32_ && !outLen_) return false;
  SQLLEN n = 0;
  bool truncated = false;
  // A negative buffer length is an application error the caller reports as HY090; treat it as
  // "no buffer" here so we never write through it.
  const SQLLEN bufLen = bufLen_ < 0 ? 0 : bufLen_;
  if (!wide_) {
    truncated = copyNarrowOut(utf8, static_cast<SQLCHAR*>(buf_), bufLen, &n, enc_);
  } else if (lengthInBytes_) {
    truncated = copyWideOutBytes(utf8, static_cast<SQLWCHAR*>(buf_), bufLen, &n);
  } else {
    truncated = copyWideOutChars(utf8, static_cast<SQLWCHAR*>(buf_), bufLen, &n);
  }
  setOutLen(n);
  return truncated;
}

SQLRETURN TextOut::writeWithDiag(std::string_view utf8, Diag& diag) {
  if (!write(utf8)) return SQL_SUCCESS;
  return diag.addWarning(sqlstate::kStringTruncated, "String data, right truncated");
}

// ---- Guards -------------------------------------------------------------------------------------

SQLRETURN guardEnv(SQLHENV h, const char* fn, const std::function<SQLRETURN(Environment&)>& body, bool keepDiag) {
  Environment* env = asEnvironment(h);
  if (!env) {
    TRINO_LOG_TRACE(kLog, "%s -> SQL_INVALID_HANDLE (env %p)", fn, h);
    return SQL_INVALID_HANDLE;
  }
  HandleLock lock(env->mutex);
  if (!keepDiag) {
    env->diag.clear();
    diag_cursor::reset(env->diag);
  }
  SQLRETURN rc = runBody(env->diag, fn, [&]() { return body(*env); });
  rc = reconcile(env->diag, rc);
  if (!keepDiag) env->diag.returnCode = rc;
  traceReturn(fn, rc, env->diag);
  return rc;
}

SQLRETURN guardDbc(SQLHDBC h, const char* fn, const std::function<SQLRETURN(Connection&)>& body, bool keepDiag) {
  Connection* c = asConnection(h);
  if (!c) {
    TRINO_LOG_TRACE(kLog, "%s -> SQL_INVALID_HANDLE (dbc %p)", fn, h);
    return SQL_INVALID_HANDLE;
  }
  HandleLock lock(c->mutex);
  if (!keepDiag) {
    c->diag.clear();
    diag_cursor::reset(c->diag);
  }
  SQLRETURN rc = runBody(c->diag, fn, [&]() { return body(*c); });
  rc = reconcile(c->diag, rc);
  if (!keepDiag) c->diag.returnCode = rc;
  traceReturn(fn, rc, c->diag);
  return rc;
}

SQLRETURN guardStmt(SQLHSTMT h, const char* fn, const std::function<SQLRETURN(Statement&)>& body, bool keepDiag) {
  Statement* s = asStatement(h);
  if (!s) {
    TRINO_LOG_TRACE(kLog, "%s -> SQL_INVALID_HANDLE (stmt %p)", fn, h);
    return SQL_INVALID_HANDLE;
  }
  // Statement calls lock only the statement (never the connection) so a long-running fetch on one
  // statement does not block attribute calls on its siblings; connect/disconnect lock both.
  HandleLock lock(s->mutex);
  // "Function in progress" marker for SQLCancel (which cancels only while callDepth > 0). The
  // outermost call starts with a clean cancel flag and clears it again when it ends: a cancel aimed
  // at an earlier call (or one the call never polled) must not abort later work such as a lazy
  // DESCRIBE, while a cancel arriving during the call is kept until the call has polled it.
  struct CallMarker {
    Statement& st;
    explicit CallMarker(Statement& x) : st(x) {
      if (st.callDepth.load() == 0) st.cancelRequested.store(false);
      st.callDepth.fetch_add(1);
    }
    ~CallMarker() {
      if (st.callDepth.fetch_sub(1) == 1) st.cancelRequested.store(false);
    }
    CallMarker(const CallMarker&) = delete;
    CallMarker& operator=(const CallMarker&) = delete;
  } marker(*s);
  if (!keepDiag) {
    s->diag.clear();
    diag_cursor::reset(s->diag);
  }
  SQLRETURN rc = runBody(s->diag, fn, [&]() { return body(*s); });
  rc = reconcile(s->diag, rc);
  if (!keepDiag) s->diag.returnCode = rc;
  traceReturn(fn, rc, s->diag);
  return rc;
}

SQLRETURN guardDesc(SQLHDESC h, const char* fn, const std::function<SQLRETURN(Descriptor&)>& body, bool keepDiag) {
  Descriptor* d = asDescriptor(h);
  if (!d) {
    TRINO_LOG_TRACE(kLog, "%s -> SQL_INVALID_HANDLE (desc %p)", fn, h);
    return SQL_INVALID_HANDLE;
  }
  // Descriptors have no mutex of their own: they are protected by their connection's mutex and,
  // because statement calls (SQLFetch reading the ARD, SQLExecute reading the APD) lock only the
  // statement, also by the mutex of every statement using the descriptor. Order: connection, then
  // statement, the same as SQLFreeHandle(STMT) and SQLDisconnect.
  std::unique_lock<std::recursive_mutex> lock;
  if (d->conn) lock = std::unique_lock<std::recursive_mutex>(d->conn->mutex);
  // The descriptor may have been freed while this call waited for the connection.
  if (asDescriptor(h) != d) {
    TRINO_LOG_TRACE(kLog, "%s -> SQL_INVALID_HANDLE (desc %p freed)", fn, h);
    return SQL_INVALID_HANDLE;
  }
  std::vector<std::unique_lock<std::recursive_mutex>> stmtLocks;
  if (d->implicitOwner) {
    stmtLocks.emplace_back(d->implicitOwner->mutex);
  } else {
    // std::set iterates in pointer order, so concurrent callers lock statements in the same order.
    for (Statement* user : d->usedBy) stmtLocks.emplace_back(user->mutex);
  }
  if (!keepDiag) {
    d->diag.clear();
    diag_cursor::reset(d->diag);
  }
  SQLRETURN rc = runBody(d->diag, fn, [&]() { return body(*d); });
  rc = reconcile(d->diag, rc);
  if (!keepDiag) d->diag.returnCode = rc;
  traceReturn(fn, rc, d->diag);
  return rc;
}

Diag* diagFor(SQLSMALLINT handleType, SQLHANDLE handle) {
  switch (handleType) {
    case SQL_HANDLE_ENV: {
      Environment* e = asEnvironment(handle);
      return e ? &e->diag : nullptr;
    }
    case SQL_HANDLE_DBC: {
      Connection* c = asConnection(handle);
      return c ? &c->diag : nullptr;
    }
    case SQL_HANDLE_STMT: {
      Statement* s = asStatement(handle);
      return s ? &s->diag : nullptr;
    }
    case SQL_HANDLE_DESC: {
      Descriptor* d = asDescriptor(handle);
      return d ? &d->diag : nullptr;
    }
    default:
      return nullptr;
  }
}

std::recursive_mutex* mutexFor(SQLSMALLINT handleType, SQLHANDLE handle) {
  switch (handleType) {
    case SQL_HANDLE_ENV: {
      Environment* e = asEnvironment(handle);
      return e ? &e->mutex : nullptr;
    }
    case SQL_HANDLE_DBC: {
      Connection* c = asConnection(handle);
      return c ? &c->mutex : nullptr;
    }
    case SQL_HANDLE_STMT: {
      Statement* s = asStatement(handle);
      return s ? &s->mutex : nullptr;
    }
    case SQL_HANDLE_DESC: {
      Descriptor* d = asDescriptor(handle);
      return (d && d->conn) ? &d->conn->mutex : nullptr;
    }
    default:
      return nullptr;
  }
}

SQLRETURN diagFromException(Diag& diag, const std::exception& e) {
  // Without RTTI the static type of `e` is all we have, so the exception being handled is rethrown
  // and re-caught by type. current_exception() is null outside a handler; then only HY000 is possible.
  const std::exception_ptr current = std::current_exception();
  if (current) {
    try {
      std::rethrow_exception(current);
    } catch (const TrinoQueryError& t) {
      return diag.addServerError(t);
    } catch (const HttpError& h) {
      return diag.addException(h);
    } catch (const CancelledError& c) {
      return diag.addException(c);
    } catch (const DriverError& d) {
      return diag.addException(d);
    } catch (const std::bad_alloc&) {
      return diag.addError(sqlstate::kMemoryAllocation, "Memory allocation error");
    } catch (const std::exception& ex) {
      return diag.addException(ex);
    } catch (...) {
      // Not a std::exception after all (cannot happen for a caller holding `e`); fall through.
    }
  }
  return diag.addException(e);
}

// ---- State checks -------------------------------------------------------------------------------

void requireConnected(const Connection& c) {
  // 08003 is what the spec prescribes for every function needing a live connection (SQLAllocHandle
  // STMT/DESC, SQLGetInfo, SQLEndTran, SQLDisconnect ...); HY010 would hide the real cause.
  if (!c.connected || !c.client) throw DriverError(sqlstate::kNotConnected, "Connection not open");
}

void requireNotBusy(const Statement& s) {
  if (s.state == StmtState::NeedData) {
    throw DriverError(sqlstate::kFunctionSequence,
                      "Function sequence error: the statement is waiting for data-at-execution parameters "
                      "(call SQLParamData/SQLPutData or SQLCancel)");
  }
  if (s.state == StmtState::Executing) {
    throw DriverError(sqlstate::kFunctionSequence, "Function sequence error: the statement is executing");
  }
}

void requireCursor(const Statement& s) {
  if (s.state != StmtState::CursorOpen && s.state != StmtState::Fetched) {
    throw DriverError(sqlstate::kInvalidCursorState, "Invalid cursor state: no cursor is open");
  }
}

void requireNoCursor(const Statement& s) {
  if (s.state == StmtState::CursorOpen || s.state == StmtState::Fetched) {
    throw DriverError(sqlstate::kInvalidCursorState,
                      "Invalid cursor state: a cursor is open on the statement (close it with SQLCloseCursor "
                      "or SQLFreeStmt(SQL_CLOSE) first)");
  }
}

AnsiEncoding ansiEncodingOf(const Connection& c) { return c.config.ansiEncoding; }

AnsiEncoding ansiEncodingOf(const Statement& s) {
  return s.conn ? ansiEncodingOf(*s.conn) : defaultAnsiEncoding();
}

}  // namespace trino_odbc
