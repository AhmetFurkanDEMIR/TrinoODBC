// Diagnostics retrieval: SQLGetDiagRec, SQLGetDiagField (ANSI entry points + shared impl:: used by
// api_unicode.cpp) and the ODBC 2 SQLError. None of these ever clears or modifies the diagnostic
// area they read (a truncated message returns SQL_SUCCESS_WITH_INFO without adding a record).
// See ODBC-API-DETAILS.md "api_diag.cpp".
#include "odbc/api_common.h"
#include "odbc/api_internal_attrs.h"

#include "util/errors.h"

#include <cstring>
#include <mutex>
#include <string>

namespace trino_odbc {

// Owned by api_common.cpp: SQLError walk position of a diagnostic area.
namespace diag_cursor {
size_t take(const Diag& d);
}

namespace {

constexpr const char* kLog = "odbc";

/// Narrow output strings use the encoding of the connection behind the handle.
AnsiEncoding ansiEncodingForHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  switch (handleType) {
    case SQL_HANDLE_DBC:
      if (Connection* c = asConnection(handle)) return ansiEncodingOf(*c);
      break;
    case SQL_HANDLE_STMT:
      if (Statement* s = asStatement(handle)) return ansiEncodingOf(*s);
      break;
    case SQL_HANDLE_DESC:
      if (Descriptor* d = asDescriptor(handle)) {
        if (d->conn) return ansiEncodingOf(*d->conn);
      }
      break;
    default:
      break;
  }
  return defaultAnsiEncoding();
}

/// Locks the mutex protecting a diagnostic area for the duration of a read (none for a handle
/// without a mutex, which cannot happen for handles that passed validation).
class DiagReadLock {
 public:
  explicit DiagReadLock(std::recursive_mutex* m) : m_(m) {
    if (m_) m_->lock();
  }
  ~DiagReadLock() {
    if (m_) m_->unlock();
  }
  DiagReadLock(const DiagReadLock&) = delete;
  DiagReadLock& operator=(const DiagReadLock&) = delete;

 private:
  std::recursive_mutex* m_;
};

template <typename T>
SQLRETURN putFixed(SQLPOINTER value, SQLSMALLINT* strLen, T v) {
  if (value) std::memcpy(value, &v, sizeof(T));
  if (strLen) *strLen = static_cast<SQLSMALLINT>(sizeof(T));
  return SQL_SUCCESS;
}

/// String diagnostic fields use the byte convention for both narrow and wide buffers
/// (SQLGetDiagFieldW documents BufferLength / *StringLengthPtr in bytes).
SQLRETURN putText(SQLPOINTER value, SQLSMALLINT bufLen, SQLSMALLINT* strLen, bool wide, AnsiEncoding enc,
                  std::string_view utf8) {
  if (bufLen < 0) return SQL_ERROR;  // HY090 cannot be posted: the area being read must stay intact
  bool truncated = false;
  if (wide) {
    TextOut out(static_cast<SQLWCHAR*>(value), bufLen, strLen, /*lengthInBytes=*/true);
    truncated = out.write(utf8);
  } else {
    TextOut out(static_cast<SQLCHAR*>(value), bufLen, strLen, enc);
    truncated = out.write(utf8);
  }
  return truncated ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

/// "ODBC 3.0" for the ODBC-defined classes (IM, HY), "ISO 9075" otherwise.
const char* classOrigin(const std::string& sqlstate) {
  if (sqlstate.size() >= 2 && ((sqlstate[0] == 'I' && sqlstate[1] == 'M') || (sqlstate[0] == 'H' && sqlstate[1] == 'Y'))) {
    return "ODBC 3.0";
  }
  return "ISO 9075";
}

/// "ODBC 3.0" for the ODBC-defined subclasses (xxSxx, HYxxx, IMxxx), "ISO 9075" otherwise.
const char* subclassOrigin(const std::string& sqlstate) {
  if (sqlstate.size() < 5) return "ISO 9075";
  if (sqlstate[2] == 'S') return "ODBC 3.0";
  if ((sqlstate[0] == 'H' && sqlstate[1] == 'Y') || (sqlstate[0] == 'I' && sqlstate[1] == 'M')) return "ODBC 3.0";
  return "ISO 9075";
}

}  // namespace

namespace impl {

SQLRETURN getDiagRec(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT rec, TextOut& sqlstate, SQLINTEGER* native,
                     TextOut& message) {
  if (rec < 1) return SQL_ERROR;
  Diag* diag = diagFor(handleType, handle);
  if (!diag) return SQL_INVALID_HANDLE;
  DiagReadLock lock(mutexFor(handleType, handle));
  const DiagRecord* r = diag->record(static_cast<size_t>(rec));
  if (!r) return SQL_NO_DATA;
  sqlstate.write(r->sqlstate);
  if (native) *native = r->native;
  const bool truncated = message.write(r->message);
  TRINO_LOG_TRACE(kLog, "SQLGetDiagRec(%d, rec %d) -> %s %s", handleType, rec, r->sqlstate.c_str(), r->message.c_str());
  return truncated ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN getDiagField(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value,
                       SQLSMALLINT bufLen, SQLSMALLINT* strLen, bool wide) {
  Diag* diag = diagFor(handleType, handle);
  if (!diag) return SQL_INVALID_HANDLE;
  const AnsiEncoding enc = ansiEncodingForHandle(handleType, handle);
  const bool isStmt = handleType == SQL_HANDLE_STMT;
  DiagReadLock lock(mutexFor(handleType, handle));

  // Header fields: RecNumber is ignored.
  switch (field) {
    case SQL_DIAG_CURSOR_ROW_COUNT:
      if (!isStmt) return SQL_ERROR;
      return putFixed<SQLLEN>(value, strLen, diag->cursorRowCount);
    case SQL_DIAG_DYNAMIC_FUNCTION:
      if (!isStmt) return SQL_ERROR;
      return putText(value, bufLen, strLen, wide, enc, diag->dynamicFunction);
    case SQL_DIAG_DYNAMIC_FUNCTION_CODE:
      if (!isStmt) return SQL_ERROR;
      return putFixed<SQLINTEGER>(value, strLen, diag->dynamicFunctionCode);
    case SQL_DIAG_NUMBER:
      return putFixed<SQLINTEGER>(value, strLen, static_cast<SQLINTEGER>(diag->count()));
    case SQL_DIAG_RETURNCODE:
      return putFixed<SQLRETURN>(value, strLen, diag->returnCode);
    case SQL_DIAG_ROW_COUNT:
      if (!isStmt) return SQL_ERROR;
      return putFixed<SQLLEN>(value, strLen, diag->rowCount);
    default:
      break;
  }

  // Record fields.
  if (rec < 1) return SQL_ERROR;
  const DiagRecord* r = diag->record(static_cast<size_t>(rec));
  if (!r) return SQL_NO_DATA;
  switch (field) {
    case SQL_DIAG_CLASS_ORIGIN: return putText(value, bufLen, strLen, wide, enc, classOrigin(r->sqlstate));
    case SQL_DIAG_SUBCLASS_ORIGIN: return putText(value, bufLen, strLen, wide, enc, subclassOrigin(r->sqlstate));
    case SQL_DIAG_COLUMN_NUMBER:
      if (!isStmt) return SQL_ERROR;
      return putFixed<SQLINTEGER>(value, strLen, r->columnNumber);
    case SQL_DIAG_ROW_NUMBER:
      if (!isStmt) return SQL_ERROR;
      return putFixed<SQLLEN>(value, strLen, r->rowNumber);
    case SQL_DIAG_CONNECTION_NAME: return putText(value, bufLen, strLen, wide, enc, r->connectionName);
    case SQL_DIAG_SERVER_NAME: return putText(value, bufLen, strLen, wide, enc, r->serverName);
    case SQL_DIAG_MESSAGE_TEXT: return putText(value, bufLen, strLen, wide, enc, r->message);
    case SQL_DIAG_NATIVE: return putFixed<SQLINTEGER>(value, strLen, r->native);
    case SQL_DIAG_SQLSTATE: return putText(value, bufLen, strLen, wide, enc, r->sqlstate);
    default:
      // HY092 cannot be posted without disturbing the area being read.
      TRINO_LOG_DEBUG(kLog, "SQLGetDiagField: unknown field identifier %d", field);
      return SQL_ERROR;
  }
}

SQLRETURN odbc2Error(SQLHENV henv, SQLHDBC hdbc, SQLHSTMT hstmt, void* sqlstateBuf, SQLINTEGER* native, void* messageBuf,
                     SQLSMALLINT bufLen, SQLSMALLINT* textLen, bool wide) {
  // ODBC 2: the most specific handle given is the one whose records are walked, one per call,
  // until SQL_NO_DATA. The walk position is reset whenever the area is cleared by a new call.
  // SQLError and SQLErrorW share this one cursor so their sequencing is identical, and the wide
  // variant writes the UTF-8 record straight into SQLWCHARs (no lossy trip through the ANSI code page).
  SQLSMALLINT type = 0;
  SQLHANDLE handle = SQL_NULL_HANDLE;
  if (hstmt != SQL_NULL_HSTMT) {
    type = SQL_HANDLE_STMT;
    handle = hstmt;
  } else if (hdbc != SQL_NULL_HDBC) {
    type = SQL_HANDLE_DBC;
    handle = hdbc;
  } else if (henv != SQL_NULL_HENV) {
    type = SQL_HANDLE_ENV;
    handle = henv;
  } else {
    return SQL_INVALID_HANDLE;
  }
  // HY090 cannot be posted: SQLError has no diagnostic area of its own to report it in.
  if (bufLen < 0) return SQL_ERROR;
  Diag* diag = diagFor(type, handle);
  if (!diag) return SQL_INVALID_HANDLE;
  const AnsiEncoding enc = ansiEncodingForHandle(type, handle);
  DiagReadLock lock(mutexFor(type, handle));
  const SQLLEN stateLen = sqlstateBuf ? SQL_SQLSTATE_SIZE + 1 : 0;
  TextOut state = wide ? TextOut(static_cast<SQLWCHAR*>(sqlstateBuf), stateLen, static_cast<SQLSMALLINT*>(nullptr), false)
                       : TextOut(static_cast<SQLCHAR*>(sqlstateBuf), stateLen, static_cast<SQLSMALLINT*>(nullptr), enc);
  TextOut message = wide ? TextOut(static_cast<SQLWCHAR*>(messageBuf), bufLen, textLen, false)
                         : TextOut(static_cast<SQLCHAR*>(messageBuf), bufLen, textLen, enc);
  const size_t rec = diag_cursor::take(*diag);
  if (rec == 0) {
    // Nothing (left) to report: the spec wants the output buffers cleared as well.
    state.write("00000");
    if (native) *native = 0;
    message.write("");
    return SQL_NO_DATA;
  }
  return getDiagRec(type, handle, static_cast<SQLSMALLINT>(rec), state, native, message);
}

}  // namespace impl
}  // namespace trino_odbc

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                               SQLCHAR* Sqlstate, SQLINTEGER* NativeError, SQLCHAR* MessageText,
                                               SQLSMALLINT BufferLength, SQLSMALLINT* TextLength) {
  if (BufferLength < 0) return SQL_ERROR;
  const AnsiEncoding enc = ansiEncodingForHandle(HandleType, Handle);
  // The SQLSTATE buffer is always 6 bytes (5 characters plus the terminator).
  TextOut state(Sqlstate, Sqlstate ? SQL_SQLSTATE_SIZE + 1 : 0, static_cast<SQLSMALLINT*>(nullptr), enc);
  TextOut message(MessageText, BufferLength, TextLength, enc);
  return impl::getDiagRec(HandleType, Handle, RecNumber, state, NativeError, message);
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT RecNumber,
                                                 SQLSMALLINT DiagIdentifier, SQLPOINTER DiagInfo, SQLSMALLINT BufferLength,
                                                 SQLSMALLINT* StringLength) {
  return impl::getDiagField(HandleType, Handle, RecNumber, DiagIdentifier, DiagInfo, BufferLength, StringLength,
                            /*wide=*/false);
}

TRINO_ODBC_API SQLRETURN SQL_API SQLError(SQLHENV EnvironmentHandle, SQLHDBC ConnectionHandle, SQLHSTMT StatementHandle,
                                          SQLCHAR* Sqlstate, SQLINTEGER* NativeError, SQLCHAR* MessageText,
                                          SQLSMALLINT BufferLength, SQLSMALLINT* TextLength) {
  try {
    return impl::odbc2Error(EnvironmentHandle, ConnectionHandle, StatementHandle, Sqlstate, NativeError, MessageText,
                            BufferLength, TextLength, /*wide=*/false);
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "SQLError failed: %s", e.what());
    return SQL_ERROR;
  }
}

}  // extern "C"
