// Environment, connection and statement attributes (SQLSet/GetEnvAttr, SQLSet/GetConnectAttr,
// SQLSet/GetStmtAttr) plus the ODBC 2 option wrappers. See ODBC-API-DETAILS.md "api_attrs.cpp".
#include "odbc/api_common.h"
#include "odbc/api_internal_attrs.h"

#include "client/trino_client.h"
#include "util/errors.h"
#include "util/strings.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "odbc";

// Integer attributes travel inside the SQLPOINTER argument itself (ODBC convention).
SQLULEN ulenOf(SQLPOINTER v) { return static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(v)); }
SQLUINTEGER uintOf(SQLPOINTER v) { return static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(v)); }
SQLINTEGER intOf(SQLPOINTER v) { return static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(v)); }

/// Writes a fixed-size attribute value; a null destination means the caller only wanted the length.
template <typename T>
SQLRETURN putValue(SQLPOINTER value, SQLINTEGER* strLen, T v) {
  if (value) std::memcpy(value, &v, sizeof(T));
  if (strLen) *strLen = static_cast<SQLINTEGER>(sizeof(T));
  return SQL_SUCCESS;
}

/// Writes a string attribute with the byte-length convention shared by SQLGetConnectAttr(W) /
/// SQLGetStmtAttr(W): BufferLength and *StringLength are bytes for both narrow and wide buffers.
SQLRETURN putString(SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide, AnsiEncoding enc,
                    std::string_view utf8, Diag& diag) {
  if (bufLen < 0) throw DriverError(sqlstate::kInvalidStringOrBufferLength, "BufferLength must not be negative");
  if (wide) {
    TextOut out(static_cast<SQLWCHAR*>(value), bufLen, strLen, /*lengthInBytes=*/true);
    return out.writeWithDiag(utf8, diag);
  }
  TextOut out(static_cast<SQLCHAR*>(value), bufLen, strLen, enc);
  return out.writeWithDiag(utf8, diag);
}

[[noreturn]] void unknownAttribute(SQLINTEGER attr) {
  throw DriverError(sqlstate::kInvalidAttribute, "Invalid attribute/option identifier " + std::to_string(attr));
}

[[noreturn]] void invalidValue(SQLINTEGER attr, SQLULEN v) {
  throw DriverError(sqlstate::kInvalidAttributeValue,
                    "Invalid attribute value " + std::to_string(v) + " for attribute " + std::to_string(attr));
}

SQLRETURN optionChanged(Diag& diag, const std::string& what) {
  return diag.addWarning(sqlstate::kOptionValueChanged, "Option value changed: " + what);
}

[[noreturn]] void notImplemented(const std::string& what) {
  throw DriverError(sqlstate::kOptionalFeatureNotImplemented, "Optional feature not implemented: " + what);
}

bool cursorIsOpen(const Statement& s) { return s.state == StmtState::CursorOpen || s.state == StmtState::Fetched; }

/// SQL_ATTR_CURRENT_CATALOG on a live connection. Trino's USE takes `catalog.schema` (a lone name
/// selects a schema), so the round trip is only possible when a schema is known; otherwise the
/// session catalog (the value sent as X-Trino-Catalog) is switched locally.
void switchCatalog(Connection& c, const std::string& catalog) {
  if (catalog.empty()) throw DriverError(sqlstate::kInvalidAttributeValue, "Catalog name must not be empty");
  const std::string schema = c.currentSchema();
  if (!c.connected || !c.client) {
    c.pendingCatalog = catalog;
    TRINO_LOG_DEBUG(kLog, "current catalog '%s' stored until connect", catalog.c_str());
    return;
  }
  if (!schema.empty()) {
    ExecOptions opts;
    opts.internal = true;
    const std::string sql = "USE " + sqlQuoteIdentifier(catalog) + "." + sqlQuoteIdentifier(schema);
    TRINO_LOG_DEBUG(kLog, "switching catalog: %s", sql.c_str());
    c.client->executeToMemory(sql, opts);  // X-Trino-Set-Catalog/Schema headers update the session
  }
  std::lock_guard<std::mutex> lock(c.session->mutex);
  c.session->catalog = catalog;  // also covers a server that does not echo Set-Catalog
}

/// ODBC 2 statement options that an ODBC 2 application may set with SQLSetConnectOption (the ids
/// coincide with the ODBC 3 statement attributes). SQL_ASYNC_ENABLE (4) is a connection attribute
/// of its own and handled separately.
bool isOdbc2StatementOption(SQLINTEGER attr) {
  switch (attr) {
    case SQL_QUERY_TIMEOUT:
    case SQL_MAX_ROWS:
    case SQL_NOSCAN:
    case SQL_MAX_LENGTH:
    case SQL_BIND_TYPE:
    case SQL_CURSOR_TYPE:
    case SQL_CONCURRENCY:
    case SQL_KEYSET_SIZE:
    case SQL_ROWSET_SIZE:
    case SQL_SIMULATE_CURSOR:
    case SQL_RETRIEVE_DATA:
    case SQL_USE_BOOKMARKS:
      return true;
    default:
      return false;
  }
}

/// SQLSetConnectOption with an ODBC 2 statement option: the option applies to every statement
/// currently allocated on the connection (ODBC 2 "statement options at the connection level").
/// Each statement's own diagnostic area is left untouched; warnings are reported on the connection.
SQLRETURN setStatementOptionOnConnection(Connection& c, SQLINTEGER attr, SQLPOINTER value);

// ---- environment --------------------------------------------------------------------------------

SQLRETURN setEnvAttr(Environment& env, SQLINTEGER attr, SQLPOINTER value) {
  const SQLINTEGER v = intOf(value);
  switch (attr) {
    case SQL_ATTR_ODBC_VERSION:
      if (v != SQL_OV_ODBC2 && v != SQL_OV_ODBC3 && v != static_cast<SQLINTEGER>(SQL_OV_ODBC3_80)) invalidValue(attr, static_cast<SQLULEN>(v));
      if (!env.connections.empty() && env.odbcVersion != v) {
        throw DriverError(sqlstate::kFunctionSequence,
                          "Function sequence error: SQL_ATTR_ODBC_VERSION cannot change once a connection handle "
                          "has been allocated on the environment");
      }
      env.odbcVersion = v;
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_POOLING:
      if (v != static_cast<SQLINTEGER>(SQL_CP_OFF) && v != static_cast<SQLINTEGER>(SQL_CP_ONE_PER_DRIVER) &&
          v != static_cast<SQLINTEGER>(SQL_CP_ONE_PER_HENV) && v != static_cast<SQLINTEGER>(SQL_CP_DRIVER_AWARE)) {
        invalidValue(attr, static_cast<SQLULEN>(v));
      }
      env.connectionPooling = v;  // pooling is a driver-manager feature; the value is only remembered
      return SQL_SUCCESS;
    case SQL_ATTR_CP_MATCH:
      if (v != static_cast<SQLINTEGER>(SQL_CP_STRICT_MATCH) && v != static_cast<SQLINTEGER>(SQL_CP_RELAXED_MATCH)) {
        invalidValue(attr, static_cast<SQLULEN>(v));
      }
      env.cpMatch = v;
      return SQL_SUCCESS;
    case SQL_ATTR_OUTPUT_NTS:
      if (v == SQL_TRUE) {
        env.outputNts = SQL_TRUE;
        return SQL_SUCCESS;
      }
      if (v == SQL_FALSE) notImplemented("SQL_ATTR_OUTPUT_NTS=SQL_FALSE (strings are always null-terminated)");
      invalidValue(attr, static_cast<SQLULEN>(v));
    default:
      unknownAttribute(attr);
  }
}

SQLRETURN getEnvAttr(Environment& env, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER* strLen) {
  switch (attr) {
    case SQL_ATTR_ODBC_VERSION: return putValue<SQLINTEGER>(value, strLen, env.odbcVersion);
    case SQL_ATTR_CONNECTION_POOLING: return putValue<SQLINTEGER>(value, strLen, env.connectionPooling);
    case SQL_ATTR_CP_MATCH: return putValue<SQLINTEGER>(value, strLen, env.cpMatch);
    case SQL_ATTR_OUTPUT_NTS: return putValue<SQLINTEGER>(value, strLen, env.outputNts);
    default: unknownAttribute(attr);
  }
}

}  // namespace

// ---- connection ---------------------------------------------------------------------------------

namespace impl {

SQLRETURN setConnectAttr(Connection& c, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER strLen, bool wide) {
  const SQLUINTEGER v = uintOf(value);
  switch (attr) {
    case SQL_ATTR_ACCESS_MODE:
      if (v != SQL_MODE_READ_WRITE && v != SQL_MODE_READ_ONLY) invalidValue(attr, v);
      c.accessMode = v;
      return SQL_SUCCESS;

    case SQL_ATTR_AUTOCOMMIT:
      if (v != SQL_AUTOCOMMIT_ON && v != SQL_AUTOCOMMIT_OFF) invalidValue(attr, v);
      if (v == SQL_AUTOCOMMIT_OFF) {
        // Before connect the data source is unknown: the request is kept and re-checked at connect.
        if (c.connected && !c.config.enableTransactions) {
          notImplemented("SQL_ATTR_AUTOCOMMIT=SQL_AUTOCOMMIT_OFF (transactions are disabled; set EnableTransactions=1)");
        }
        c.autocommit = SQL_AUTOCOMMIT_OFF;
        return SQL_SUCCESS;
      }
      if (c.autocommit == SQL_AUTOCOMMIT_OFF && c.inTransaction) {
        // Turning autocommit back on commits the open transaction (ODBC semantics).
        std::string txId;
        {
          std::lock_guard<std::mutex> lock(c.session->mutex);
          txId = c.session->transactionId;
        }
        if (c.client) {
          ExecOptions opts;
          opts.internal = true;
          if (!txId.empty()) opts.transactionId = txId;
          c.client->executeToMemory("COMMIT", opts);
        }
        std::lock_guard<std::mutex> lock(c.session->mutex);
        c.session->transactionId.clear();
        c.inTransaction = false;
      }
      c.autocommit = SQL_AUTOCOMMIT_ON;
      return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_TIMEOUT:
      c.connectionTimeout = v;
      return SQL_SUCCESS;

    case SQL_ATTR_LOGIN_TIMEOUT:
      // Only meaningful before the connection is established; afterwards the value is kept for
      // SQLGetConnectAttr but has no effect (applications such as Power BI set it at various times).
      c.loginTimeout = v;
      return SQL_SUCCESS;

    case SQL_ATTR_CURRENT_CATALOG: {
      std::optional<std::string> catalog = textInBytes(value, strLen, wide, ansiEncodingOf(c));
      if (!catalog) throw DriverError(sqlstate::kInvalidNullPointer, "SQL_ATTR_CURRENT_CATALOG value is a null pointer");
      switchCatalog(c, std::string(trim(*catalog)));
      return SQL_SUCCESS;
    }

    case SQL_ATTR_METADATA_ID:
      if (v != SQL_TRUE && v != SQL_FALSE) invalidValue(attr, v);
      c.metadataId = v;
      return SQL_SUCCESS;

    case SQL_ATTR_PACKET_SIZE:
      c.packetSize = v;
      return optionChanged(c.diag, "SQL_ATTR_PACKET_SIZE is not used by the HTTP transport");

    case SQL_ATTR_QUIET_MODE:
      c.quietMode = value;
      return SQL_SUCCESS;

    case SQL_ATTR_TRACE:
      if (v != SQL_OPT_TRACE_OFF && v != SQL_OPT_TRACE_ON) invalidValue(attr, v);
      c.trace = v;  // tracing is performed by the driver manager
      return SQL_SUCCESS;

    case SQL_ATTR_TRACEFILE: {
      std::optional<std::string> file = textInBytes(value, strLen, wide, ansiEncodingOf(c));
      c.traceFile = file.value_or(std::string());
      return SQL_SUCCESS;
    }

    case SQL_ATTR_TXN_ISOLATION:
      if (v != SQL_TXN_READ_UNCOMMITTED && v != SQL_TXN_READ_COMMITTED && v != SQL_TXN_REPEATABLE_READ &&
          v != SQL_TXN_SERIALIZABLE) {
        invalidValue(attr, v);
      }
      if (c.inTransaction) {
        throw DriverError(sqlstate::kAttributeCannotBeSetNow,
                          "SQL_ATTR_TXN_ISOLATION cannot be changed while a transaction is open");
      }
      c.txnIsolation = v;
      return SQL_SUCCESS;

    case SQL_ATTR_ASYNC_ENABLE:
      if (v == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
      if (v != SQL_ASYNC_ENABLE_ON) invalidValue(attr, v);
      c.asyncEnable = SQL_ASYNC_ENABLE_OFF;
      return optionChanged(c.diag, "asynchronous execution is not supported; SQL_ATTR_ASYNC_ENABLE stays off");

    case SQL_ATTR_ASYNC_DBC_FUNCTIONS:
      if (v == static_cast<SQLUINTEGER>(SQL_ASYNC_DBC_ENABLE_OFF)) return SQL_SUCCESS;
      return optionChanged(c.diag, "asynchronous connection functions are not supported");

    case SQL_ATTR_ODBC_CURSORS:
      c.odbcCursors = v;  // interpreted by the driver manager
      return SQL_SUCCESS;

    case SQL_ATTR_ENLIST_IN_DTC:
      notImplemented("SQL_ATTR_ENLIST_IN_DTC (distributed transactions)");

    case SQL_ATTR_ANSI_APP:
      return SQL_SUCCESS;  // both ANSI and Unicode applications are served natively

    case SQL_ATTR_RESET_CONNECTION:
      if (v != static_cast<SQLUINTEGER>(SQL_RESET_CONNECTION_YES)) invalidValue(attr, v);
      // An ODBC 3.8 driver manager sets this just before pooling the connection and re-uses it later
      // without reconnecting: the previous user's session state must not leak to the next one.
      impl::resetPooledConnection(c);
      return SQL_SUCCESS;

    case SQL_ATTR_TRANSLATE_LIB:
    case SQL_ATTR_TRANSLATE_OPTION:
      notImplemented("translation libraries (SQL_ATTR_TRANSLATE_LIB/SQL_ATTR_TRANSLATE_OPTION)");

    case SQL_ATTR_DBC_INFO_TOKEN:
    case SQL_ATTR_ASYNC_DBC_EVENT:
      notImplemented("driver-aware pooling / asynchronous notification attributes");

    case SQL_ATTR_AUTO_IPD:
    case SQL_ATTR_CONNECTION_DEAD:
      throw DriverError(sqlstate::kInvalidAttribute,
                        "Attribute " + std::to_string(attr) + " is read-only");

    default:
      if (isOdbc2StatementOption(attr)) return setStatementOptionOnConnection(c, attr, value);
      unknownAttribute(attr);
  }
}

SQLRETURN getConnectAttr(Connection& c, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide) {
  const AnsiEncoding enc = ansiEncodingOf(c);
  switch (attr) {
    case SQL_ATTR_ACCESS_MODE: return putValue<SQLUINTEGER>(value, strLen, c.accessMode);
    case SQL_ATTR_AUTOCOMMIT: return putValue<SQLUINTEGER>(value, strLen, c.autocommit);
    case SQL_ATTR_CONNECTION_TIMEOUT: return putValue<SQLUINTEGER>(value, strLen, c.connectionTimeout);
    case SQL_ATTR_LOGIN_TIMEOUT: return putValue<SQLUINTEGER>(value, strLen, c.loginTimeout);
    case SQL_ATTR_CURRENT_CATALOG: return putString(value, bufLen, strLen, wide, enc, c.currentCatalog(), c.diag);
    case SQL_ATTR_METADATA_ID: return putValue<SQLUINTEGER>(value, strLen, c.metadataId);
    case SQL_ATTR_PACKET_SIZE: return putValue<SQLUINTEGER>(value, strLen, c.packetSize);
    case SQL_ATTR_QUIET_MODE: return putValue<SQLPOINTER>(value, strLen, c.quietMode);
    case SQL_ATTR_TRACE: return putValue<SQLUINTEGER>(value, strLen, c.trace);
    case SQL_ATTR_TRACEFILE: return putString(value, bufLen, strLen, wide, enc, c.traceFile, c.diag);
    case SQL_ATTR_TXN_ISOLATION: return putValue<SQLUINTEGER>(value, strLen, c.txnIsolation);
    case SQL_ATTR_ASYNC_ENABLE: return putValue<SQLUINTEGER>(value, strLen, static_cast<SQLUINTEGER>(SQL_ASYNC_ENABLE_OFF));
    case SQL_ATTR_ASYNC_DBC_FUNCTIONS:
      return putValue<SQLUINTEGER>(value, strLen, static_cast<SQLUINTEGER>(SQL_ASYNC_DBC_ENABLE_OFF));
    case SQL_ATTR_ODBC_CURSORS: return putValue<SQLUINTEGER>(value, strLen, c.odbcCursors);
    case SQL_ATTR_AUTO_IPD: return putValue<SQLUINTEGER>(value, strLen, static_cast<SQLUINTEGER>(SQL_TRUE));
    case SQL_ATTR_CONNECTION_DEAD:
      return putValue<SQLUINTEGER>(value, strLen,
                                   static_cast<SQLUINTEGER>((c.connected && c.client) ? SQL_CD_FALSE : SQL_CD_TRUE));
    case SQL_ATTR_ANSI_APP: return putValue<SQLUINTEGER>(value, strLen, static_cast<SQLUINTEGER>(SQL_AA_FALSE));
    case SQL_ATTR_RESET_CONNECTION: return putValue<SQLUINTEGER>(value, strLen, 0);
    case SQL_ATTR_ENLIST_IN_DTC:
      notImplemented("SQL_ATTR_ENLIST_IN_DTC (distributed transactions)");
    case SQL_ATTR_TRANSLATE_LIB:
    case SQL_ATTR_TRANSLATE_OPTION:
      notImplemented("translation libraries (SQL_ATTR_TRANSLATE_LIB/SQL_ATTR_TRANSLATE_OPTION)");
    case SQL_ATTR_DBC_INFO_TOKEN:
    case SQL_ATTR_ASYNC_DBC_EVENT:
      notImplemented("driver-aware pooling / asynchronous notification attributes");
    default:
      unknownAttribute(attr);
  }
}

// ---- statement ----------------------------------------------------------------------------------

SQLRETURN setStmtAttr(Statement& s, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER strLen, bool wide) {
  (void)strLen;
  (void)wide;  // no statement attribute carries a string
  requireNotBusy(s);
  const SQLULEN v = ulenOf(value);

  switch (attr) {
    case SQL_ATTR_CONCURRENCY:
    case SQL_ATTR_CURSOR_TYPE:
    case SQL_ATTR_SIMULATE_CURSOR:
    case SQL_ATTR_USE_BOOKMARKS:
    case SQL_ATTR_CURSOR_SCROLLABLE:
    case SQL_ATTR_CURSOR_SENSITIVITY:
    case SQL_ATTR_KEYSET_SIZE:
      if (cursorIsOpen(s)) {
        throw DriverError(sqlstate::kInvalidCursorState,
                          "Invalid cursor state: cursor attributes cannot be changed while a cursor is open");
      }
      break;
    default:
      break;
  }

  switch (attr) {
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC: {
      const bool row = attr == SQL_ATTR_APP_ROW_DESC;
      Descriptor*& slot = row ? s.ard : s.apd;
      Descriptor* implicitDesc = row ? &s.implicitArd : &s.implicitApd;
      Descriptor* target = nullptr;
      if (value == SQL_NULL_HDESC) {
        target = implicitDesc;
      } else {
        target = asDescriptor(value);
        if (!target) {
          throw DriverError(sqlstate::kInvalidAttributeValue,
                            "Invalid attribute value: the descriptor handle is not valid");
        }
        if (!target->isExplicit()) {
          // Only the statement's own implicit descriptor of the same kind may be re-attached.
          if (target != implicitDesc) {
            throw DriverError(sqlstate::kInvalidUseOfDescriptor,
                              "Invalid use of an automatically allocated descriptor handle: an implicit descriptor "
                              "of another statement (or an IRD/IPD) cannot be used as an application descriptor");
          }
        } else if (target->conn != s.conn) {
          throw DriverError(sqlstate::kInvalidAttributeValue,
                            "Invalid attribute value: the descriptor belongs to a different connection");
        }
      }
      if (slot == target) return SQL_SUCCESS;
      if (slot && slot->isExplicit()) slot->usedBy.erase(&s);
      if (target->isExplicit()) {
        target->usedBy.insert(&s);
        target->kind = row ? DescKind::Ard : DescKind::Apd;
      }
      slot = target;
      return SQL_SUCCESS;
    }

    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
      throw DriverError(sqlstate::kInvalidUseOfDescriptor,
                        "Invalid use of an automatically allocated descriptor handle: the IRD and IPD are read-only");

    case SQL_ATTR_ASYNC_ENABLE:
      if (v == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
      if (v != SQL_ASYNC_ENABLE_ON) invalidValue(attr, v);
      s.asyncEnable = SQL_ASYNC_ENABLE_OFF;
      return optionChanged(s.diag, "asynchronous execution is not supported; SQL_ATTR_ASYNC_ENABLE stays off");

    case SQL_ATTR_ASYNC_STMT_EVENT:
      notImplemented("SQL_ATTR_ASYNC_STMT_EVENT (asynchronous notification)");

    case SQL_ATTR_CONCURRENCY:
      if (v == SQL_CONCUR_READ_ONLY) {
        s.concurrency = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_CONCUR_LOCK && v != SQL_CONCUR_ROWVER && v != SQL_CONCUR_VALUES) invalidValue(attr, v);
      s.concurrency = SQL_CONCUR_READ_ONLY;
      return optionChanged(s.diag, "cursors are read-only; SQL_ATTR_CONCURRENCY set to SQL_CONCUR_READ_ONLY");

    case SQL_ATTR_CURSOR_SCROLLABLE:
      if (v == SQL_NONSCROLLABLE) {
        s.cursorScrollable = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_SCROLLABLE) invalidValue(attr, v);
      s.cursorScrollable = SQL_NONSCROLLABLE;
      return optionChanged(s.diag, "cursors are forward-only; SQL_ATTR_CURSOR_SCROLLABLE set to SQL_NONSCROLLABLE");

    case SQL_ATTR_CURSOR_SENSITIVITY:
      if (v == SQL_UNSPECIFIED || v == SQL_INSENSITIVE) {
        s.cursorSensitivity = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_SENSITIVE) invalidValue(attr, v);
      s.cursorSensitivity = SQL_UNSPECIFIED;
      return optionChanged(s.diag, "sensitive cursors are not supported; SQL_ATTR_CURSOR_SENSITIVITY set to SQL_UNSPECIFIED");

    case SQL_ATTR_CURSOR_TYPE:
      if (v == SQL_CURSOR_FORWARD_ONLY) {
        s.cursorType = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_CURSOR_STATIC && v != SQL_CURSOR_KEYSET_DRIVEN && v != SQL_CURSOR_DYNAMIC) invalidValue(attr, v);
      s.cursorType = SQL_CURSOR_FORWARD_ONLY;
      return optionChanged(s.diag, "only forward-only cursors are supported; SQL_ATTR_CURSOR_TYPE set to SQL_CURSOR_FORWARD_ONLY");

    case SQL_ATTR_ENABLE_AUTO_IPD:
      if (v != SQL_TRUE && v != SQL_FALSE) invalidValue(attr, v);
      // The IPD is always populated from DESCRIBE INPUT; there is nothing to switch off.
      return SQL_SUCCESS;

    case SQL_ATTR_FETCH_BOOKMARK_PTR:
      notImplemented("bookmarks (SQL_ATTR_FETCH_BOOKMARK_PTR)");

    case SQL_ATTR_KEYSET_SIZE:
      if (v == 0) {
        s.keysetSize = 0;
        return SQL_SUCCESS;
      }
      s.keysetSize = 0;
      return optionChanged(s.diag, "keyset cursors are not supported; SQL_ATTR_KEYSET_SIZE set to 0");

    case SQL_ATTR_MAX_LENGTH:
      s.maxLength = v;
      return SQL_SUCCESS;

    case SQL_ATTR_MAX_ROWS:
      s.maxRows = v;
      return SQL_SUCCESS;

    case SQL_ATTR_METADATA_ID:
      if (v != SQL_TRUE && v != SQL_FALSE) invalidValue(attr, v);
      s.metadataId = v;
      return SQL_SUCCESS;

    case SQL_ATTR_NOSCAN:
      if (v != SQL_NOSCAN_OFF && v != SQL_NOSCAN_ON) invalidValue(attr, v);
      s.noscan = v;
      return SQL_SUCCESS;

    case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
      s.apd->header.bindOffsetPtr = static_cast<SQLLEN*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_PARAM_BIND_TYPE:
      s.apd->header.bindType = static_cast<SQLINTEGER>(v);
      return SQL_SUCCESS;
    case SQL_ATTR_PARAM_OPERATION_PTR:
      s.apd->header.arrayStatusPtr = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_PARAM_STATUS_PTR:
      s.ipd->header.arrayStatusPtr = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_PARAMS_PROCESSED_PTR:
      s.ipd->header.rowsProcessedPtr = static_cast<SQLULEN*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_PARAMSET_SIZE:
      if (v == 0) invalidValue(attr, v);
      s.apd->header.arraySize = v;
      return SQL_SUCCESS;

    case SQL_ATTR_QUERY_TIMEOUT:
      s.queryTimeout = v;
      return SQL_SUCCESS;

    case SQL_ATTR_RETRIEVE_DATA:
      if (v != SQL_RD_ON && v != SQL_RD_OFF) invalidValue(attr, v);
      s.retrieveData = v;
      return SQL_SUCCESS;

    case SQL_ATTR_ROW_ARRAY_SIZE:
      if (v == 0) invalidValue(attr, v);
      s.ard->header.arraySize = v;
      return SQL_SUCCESS;

    case SQL_ROWSET_SIZE:
      // ODBC 2 rowset size, used only by SQLExtendedFetch; SQLFetch/SQLFetchScroll keep using the
      // ARD array size, so single-row buffers bound for SQLFetch are never overrun.
      if (v == 0) invalidValue(attr, v);
      s.odbc2RowsetSize = v;
      return SQL_SUCCESS;

    case SQL_ATTR_ROW_BIND_OFFSET_PTR:
      s.ard->header.bindOffsetPtr = static_cast<SQLLEN*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_BIND_TYPE:
      s.ard->header.bindType = static_cast<SQLINTEGER>(v);
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_OPERATION_PTR:
      s.ard->header.arrayStatusPtr = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_ROW_STATUS_PTR:
      s.ird->header.arrayStatusPtr = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_ROWS_FETCHED_PTR:
      s.ird->header.rowsProcessedPtr = static_cast<SQLULEN*>(value);
      return SQL_SUCCESS;

    case SQL_ATTR_ROW_NUMBER:
      throw DriverError(sqlstate::kInvalidAttribute, "SQL_ATTR_ROW_NUMBER is read-only");

    case SQL_ATTR_SIMULATE_CURSOR:
      if (v == SQL_SC_NON_UNIQUE) {
        s.simulateCursor = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_SC_TRY_UNIQUE && v != SQL_SC_UNIQUE) invalidValue(attr, v);
      s.simulateCursor = SQL_SC_NON_UNIQUE;
      return optionChanged(s.diag, "positioned updates are not supported; SQL_ATTR_SIMULATE_CURSOR set to SQL_SC_NON_UNIQUE");

    case SQL_ATTR_USE_BOOKMARKS:
      if (v == SQL_UB_OFF) {
        s.useBookmarks = v;
        return SQL_SUCCESS;
      }
      if (v != SQL_UB_FIXED && v != SQL_UB_VARIABLE) invalidValue(attr, v);
      s.useBookmarks = SQL_UB_OFF;
      return optionChanged(s.diag, "bookmarks are not supported; SQL_ATTR_USE_BOOKMARKS set to SQL_UB_OFF");

    default:
      unknownAttribute(attr);
  }
}

SQLRETURN getStmtAttr(Statement& s, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER bufLen, SQLINTEGER* strLen, bool wide) {
  (void)bufLen;
  (void)wide;  // no statement attribute carries a string
  switch (attr) {
    case SQL_ATTR_APP_ROW_DESC: return putValue<SQLHDESC>(value, strLen, s.ard);
    case SQL_ATTR_APP_PARAM_DESC: return putValue<SQLHDESC>(value, strLen, s.apd);
    case SQL_ATTR_IMP_ROW_DESC: return putValue<SQLHDESC>(value, strLen, s.ird);
    case SQL_ATTR_IMP_PARAM_DESC: return putValue<SQLHDESC>(value, strLen, s.ipd);
    case SQL_ATTR_ASYNC_ENABLE: return putValue<SQLULEN>(value, strLen, static_cast<SQLULEN>(SQL_ASYNC_ENABLE_OFF));
    case SQL_ATTR_CONCURRENCY: return putValue<SQLULEN>(value, strLen, s.concurrency);
    case SQL_ATTR_CURSOR_SCROLLABLE: return putValue<SQLULEN>(value, strLen, s.cursorScrollable);
    case SQL_ATTR_CURSOR_SENSITIVITY: return putValue<SQLULEN>(value, strLen, s.cursorSensitivity);
    case SQL_ATTR_CURSOR_TYPE: return putValue<SQLULEN>(value, strLen, s.cursorType);
    case SQL_ATTR_ENABLE_AUTO_IPD: return putValue<SQLULEN>(value, strLen, static_cast<SQLULEN>(SQL_TRUE));
    case SQL_ATTR_FETCH_BOOKMARK_PTR: return putValue<SQLPOINTER>(value, strLen, nullptr);
    case SQL_ATTR_KEYSET_SIZE: return putValue<SQLULEN>(value, strLen, s.keysetSize);
    case SQL_ATTR_MAX_LENGTH: return putValue<SQLULEN>(value, strLen, s.maxLength);
    case SQL_ATTR_MAX_ROWS: return putValue<SQLULEN>(value, strLen, s.maxRows);
    case SQL_ATTR_METADATA_ID: return putValue<SQLULEN>(value, strLen, s.metadataId);
    case SQL_ATTR_NOSCAN: return putValue<SQLULEN>(value, strLen, s.noscan);
    case SQL_ATTR_PARAM_BIND_OFFSET_PTR: return putValue<SQLLEN*>(value, strLen, s.apd->header.bindOffsetPtr);
    case SQL_ATTR_PARAM_BIND_TYPE: return putValue<SQLULEN>(value, strLen, static_cast<SQLULEN>(s.apd->header.bindType));
    case SQL_ATTR_PARAM_OPERATION_PTR: return putValue<SQLUSMALLINT*>(value, strLen, s.apd->header.arrayStatusPtr);
    case SQL_ATTR_PARAM_STATUS_PTR: return putValue<SQLUSMALLINT*>(value, strLen, s.ipd->header.arrayStatusPtr);
    case SQL_ATTR_PARAMS_PROCESSED_PTR: return putValue<SQLULEN*>(value, strLen, s.ipd->header.rowsProcessedPtr);
    case SQL_ATTR_PARAMSET_SIZE: return putValue<SQLULEN>(value, strLen, s.apd->header.arraySize);
    case SQL_ATTR_QUERY_TIMEOUT: return putValue<SQLULEN>(value, strLen, s.queryTimeout);
    case SQL_ATTR_RETRIEVE_DATA: return putValue<SQLULEN>(value, strLen, s.retrieveData);
    case SQL_ATTR_ROW_ARRAY_SIZE: return putValue<SQLULEN>(value, strLen, s.ard->header.arraySize);
    case SQL_ROWSET_SIZE: return putValue<SQLULEN>(value, strLen, s.odbc2RowsetSize);
    case SQL_ATTR_ROW_BIND_OFFSET_PTR: return putValue<SQLLEN*>(value, strLen, s.ard->header.bindOffsetPtr);
    case SQL_ATTR_ROW_BIND_TYPE: return putValue<SQLULEN>(value, strLen, static_cast<SQLULEN>(s.ard->header.bindType));
    case SQL_ATTR_ROW_OPERATION_PTR: return putValue<SQLUSMALLINT*>(value, strLen, s.ard->header.arrayStatusPtr);
    case SQL_ATTR_ROW_STATUS_PTR: return putValue<SQLUSMALLINT*>(value, strLen, s.ird->header.arrayStatusPtr);
    case SQL_ATTR_ROWS_FETCHED_PTR: return putValue<SQLULEN*>(value, strLen, s.ird->header.rowsProcessedPtr);
    case SQL_ATTR_ROW_NUMBER: {
      // Number of the current row within the whole result: rowset start + position inside it.
      SQLULEN n = 0;
      if (s.state == StmtState::Fetched && s.rowNumber > 0) {
        n = s.rowNumber + static_cast<SQLULEN>(s.rowsetPos > 0 ? s.rowsetPos - 1 : 0);
      }
      return putValue<SQLULEN>(value, strLen, n);
    }
    case SQL_ATTR_SIMULATE_CURSOR: return putValue<SQLULEN>(value, strLen, s.simulateCursor);
    case SQL_ATTR_USE_BOOKMARKS: return putValue<SQLULEN>(value, strLen, s.useBookmarks);
    case SQL_ATTR_ASYNC_STMT_EVENT:
      notImplemented("SQL_ATTR_ASYNC_STMT_EVENT (asynchronous notification)");
    default:
      unknownAttribute(attr);
  }
}

}  // namespace impl

namespace {

SQLRETURN setStatementOptionOnConnection(Connection& c, SQLINTEGER attr, SQLPOINTER value) {
  SQLRETURN rc = SQL_SUCCESS;
  for (Statement* st : c.statements) {
    HandleLock lock(st->mutex);  // connection (held by the guard) before statement
    Diag saved = st->diag;
    st->diag.clear();
    try {
      rc = mergeReturnCode(rc, impl::setStmtAttr(*st, attr, value, 0, /*wide=*/false));
    } catch (...) {
      st->diag = std::move(saved);
      throw;
    }
    for (const DiagRecord& r : st->diag.records()) {
      bool duplicate = false;  // the same 01S02 would otherwise be repeated once per statement
      for (const DiagRecord& existing : c.diag.records()) {
        if (existing.sqlstate == r.sqlstate && existing.message == r.message) duplicate = true;
      }
      if (!duplicate) c.diag.add(r);
    }
    st->diag = std::move(saved);
  }
  TRINO_LOG_DEBUG(kLog, "ODBC 2 statement option %d set on %zu statement(s) of the connection", static_cast<int>(attr),
                  c.statements.size());
  return rc;
}

/// SQLGetStmtOption: ODBC 2 options are 32-bit UDWORDs except these four, which the 64-bit ODBC
/// rules widen to SQLULEN.
bool isWideStmtOption(SQLUSMALLINT option) {
  return option == SQL_KEYSET_SIZE || option == SQL_MAX_LENGTH || option == SQL_MAX_ROWS || option == SQL_ROWSET_SIZE;
}

/// Ids defined by ODBC 2 for SQLGetStmtOption (SQL_QUERY_TIMEOUT .. SQL_ROW_NUMBER).
bool isOdbc2StmtOptionId(SQLUSMALLINT option) { return option <= SQL_ROW_NUMBER; }

/// ODBC 2 connection options carrying a string pass a pointer to a null-terminated narrow string.
bool isStringConnectOption(SQLUSMALLINT option) {
  return option == SQL_ATTR_CURRENT_CATALOG || option == SQL_ATTR_TRACEFILE || option == SQL_ATTR_TRANSLATE_LIB;
}

}  // namespace
}  // namespace trino_odbc

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                               SQLINTEGER StringLength) {
  (void)StringLength;
  return guardEnv(EnvironmentHandle, "SQLSetEnvAttr",
                  [&](Environment& env) -> SQLRETURN { return setEnvAttr(env, Attribute, Value); });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                               SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  (void)BufferLength;
  return guardEnv(EnvironmentHandle, "SQLGetEnvAttr",
                  [&](Environment& env) -> SQLRETURN { return getEnvAttr(env, Attribute, Value, StringLength); });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                                   SQLINTEGER StringLength) {
  return guardDbc(ConnectionHandle, "SQLSetConnectAttr", [&](Connection& c) -> SQLRETURN {
    return impl::setConnectAttr(c, Attribute, Value, StringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                                   SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  return guardDbc(ConnectionHandle, "SQLGetConnectAttr", [&](Connection& c) -> SQLRETURN {
    return impl::getConnectAttr(c, Attribute, Value, BufferLength, StringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetConnectOption(SQLHDBC ConnectionHandle, SQLUSMALLINT Option, SQLULEN Value) {
  return guardDbc(ConnectionHandle, "SQLSetConnectOption", [&](Connection& c) -> SQLRETURN {
    // ODBC 2 option ids coincide with the ODBC 3 attribute ids; strings arrive as pointers.
    SQLPOINTER ptr = reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(Value));
    const SQLINTEGER len = isStringConnectOption(Option) ? SQL_NTS : 0;
    return impl::setConnectAttr(c, static_cast<SQLINTEGER>(Option), ptr, len, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetConnectOption(SQLHDBC ConnectionHandle, SQLUSMALLINT Option, SQLPOINTER Value) {
  return guardDbc(ConnectionHandle, "SQLGetConnectOption", [&](Connection& c) -> SQLRETURN {
    // ODBC 2 has no buffer length: string options assume SQL_MAX_OPTION_STRING_LENGTH bytes.
    const SQLINTEGER bufLen = isStringConnectOption(Option) ? SQL_MAX_OPTION_STRING_LENGTH : 0;
    return impl::getConnectAttr(c, static_cast<SQLINTEGER>(Option), Value, bufLen, nullptr, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                                SQLINTEGER StringLength) {
  return guardStmt(StatementHandle, "SQLSetStmtAttr", [&](Statement& s) -> SQLRETURN {
    return impl::setStmtAttr(s, Attribute, Value, StringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute, SQLPOINTER Value,
                                                SQLINTEGER BufferLength, SQLINTEGER* StringLength) {
  return guardStmt(StatementHandle, "SQLGetStmtAttr", [&](Statement& s) -> SQLRETURN {
    return impl::getStmtAttr(s, Attribute, Value, BufferLength, StringLength, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetStmtOption(SQLHSTMT StatementHandle, SQLUSMALLINT Option, SQLULEN Value) {
  return guardStmt(StatementHandle, "SQLSetStmtOption", [&](Statement& s) -> SQLRETURN {
    // SQL_ROWSET_SIZE stays SQL_ROWSET_SIZE: it is the SQLExtendedFetch rowset, not the ARD array size.
    SQLPOINTER ptr = reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(Value));
    return impl::setStmtAttr(s, static_cast<SQLINTEGER>(Option), ptr, 0, /*wide=*/false);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetStmtOption(SQLHSTMT StatementHandle, SQLUSMALLINT Option, SQLPOINTER Value) {
  return guardStmt(StatementHandle, "SQLGetStmtOption", [&](Statement& s) -> SQLRETURN {
    // Read into a local first: ODBC 2 applications pass a 4-byte buffer for every option except the
    // four 64-bit ones, so writing a full SQLULEN would overrun it on LP64 / Win64.
    alignas(SQLULEN) unsigned char local[sizeof(SQLULEN) > sizeof(SQLPOINTER) ? sizeof(SQLULEN) : sizeof(SQLPOINTER)] = {};
    SQLINTEGER written = 0;
    const SQLRETURN rc = impl::getStmtAttr(s, static_cast<SQLINTEGER>(Option), local, 0, &written, /*wide=*/false);
    if (!SQL_SUCCEEDED(rc) || Value == nullptr) return rc;
    if (isOdbc2StmtOptionId(Option) && !isWideStmtOption(Option) && written == static_cast<SQLINTEGER>(sizeof(SQLULEN))) {
      SQLULEN full = 0;
      std::memcpy(&full, local, sizeof(full));
      const SQLUINTEGER narrowValue = static_cast<SQLUINTEGER>(full);
      std::memcpy(Value, &narrowValue, sizeof(narrowValue));
    } else if (written > 0) {
      std::memcpy(Value, local, static_cast<size_t>(written));
    }
    return rc;
  });
}

}  // extern "C"
