// Statement entry points: SQLPrepare / SQLExecute / SQLExecDirect, parameter binding and
// data-at-execution, DESCRIBE INPUT/OUTPUT metadata caching, cancel, cursor names and the
// small statement-level housekeeping calls (SQLRowCount, SQLMoreResults, SQLFreeStmt ...).
// See docs/design/ODBC-API-DETAILS.md "api_statement" and ARCHITECTURE.md §8.
#include "odbc/api_common.h"
#include "odbc/stmt_results.h"

#include "convert/params.h"
#include "sql/escape.h"
#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/strings.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "stmt";

/// Server-side prepared statement names must be unique per session; a process-wide counter keeps
/// them unique across connections too, so a pooled session never sees a name reused.
std::atomic<unsigned long long> g_prepCounter{0};

// ---- SharedQueryRowSource -------------------------------------------------------------------------
/// ResultSet owns its RowSource through a unique_ptr while SQLCancel needs a shared handle on the
/// same QueryExecution (Statement::activeQuery). This adapter forwards every RowSource call to the
/// shared execution so both owners refer to one query.
class SharedQueryRowSource : public RowSource {
 public:
  explicit SharedQueryRowSource(std::shared_ptr<QueryExecution> q) : q_(std::move(q)) {}
  const std::vector<ResultColumn>& columns() const override { return q_->columns(); }
  std::unique_ptr<Page> nextPage() override { return q_->nextPage(); }
  void cancel() override { q_->cancel(); }
  void close() override { q_->close(); }
  std::optional<int64_t> updateCount() const override { return q_->updateCount(); }
  std::string updateType() const override { return q_->updateType(); }
  const std::vector<QueryWarning>& warnings() const override { return q_->warnings(); }
  std::string queryId() const override { return q_->queryId(); }

 private:
  std::shared_ptr<QueryExecution> q_;
};

// ---- small helpers --------------------------------------------------------------------------------

/// Cancels the function in progress on `s` without its mutex: raises the cancel flag the running
/// call polls and cancels the query it is working on (wakes a blocked execute or fetch).
void cancelStatementInProgress(Statement& s) {
  s.cancelRequested.store(true);
  std::shared_ptr<QueryExecution> q;
  {
    std::lock_guard<std::mutex> g(s.activeQueryMutex);
    q = s.activeQuery;
  }
  if (!q) return;
  try {
    q->cancel();
  } catch (const std::exception& e) {
    TRINO_LOG_WARN(kLog, "cancelling query %s failed: %s", q->queryId().c_str(), e.what());
  }
}

/// Cancels the statement's open query on the server when it is still running there (the prefetch
/// thread streams it in the background, or waits for the application to make room). Works without
/// the statement mutex, like cancelStatementInProgress(). The next fetch on the cursor then fails
/// with HY008; a result that was already downloaded completely is left alone. Returns whether a
/// query was cancelled.
bool cancelRunningQuery(Statement& s) {
  std::shared_ptr<QueryExecution> q;
  {
    std::lock_guard<std::mutex> g(s.activeQueryMutex);
    q = s.activeQuery;
  }
  if (!q) return false;
  try {
    if (!q->cancelIfRunning()) return false;
    // Applied to the cursor at the next call entry (StatementCallTracker): the next fetch reports
    // HY008 even while rows of the cancelled query are still buffered.
    s.callDepth.activity()->cancelPending.store(true);
    return true;
  } catch (const std::exception& e) {
    TRINO_LOG_WARN(kLog, "cancelling query %s failed: %s", q->queryId().c_str(), e.what());
    return false;
  }
}

/// State a statement falls back to when an execution fails or is abandoned.
StmtState idleState(const Statement& s) { return s.prepared ? StmtState::Prepared : StmtState::Allocated; }

/// Restores the pre-execution state unless the execution committed a new one.
struct StateGuard {
  Statement& s;
  StmtState fallback;
  bool committed = false;
  ~StateGuard() {
    if (!committed) {
      s.state = fallback;
      s.dae = DaeState{};
      s.currentParamSet = 0;
    }
  }
};

/// Address of element `index` of an application array bound column-wise (stride = element size)
/// or row-wise (stride = SQL_DESC_BIND_TYPE), plus the SQL_DESC_BIND_OFFSET_PTR value.
void* elementAddress(void* base, SQLINTEGER bindType, SQLLEN offset, SQLULEN index, size_t elementSize) {
  if (!base) return nullptr;
  const size_t stride = bindType == SQL_BIND_BY_COLUMN ? elementSize : static_cast<size_t>(bindType);
  return static_cast<char*>(base) + offset + static_cast<size_t>(index) * stride;
}

bool isDataAtExecLength(SQLLEN len) { return len == SQL_DATA_AT_EXEC || len <= SQL_LEN_DATA_AT_EXEC_OFFSET; }

/// SQL type, column size and decimal digits of parameter `index1`. The IPD is the authoritative
/// description (SQLBindParameter writes it, and the application may change it afterwards with
/// SQLSetDescField / SQLSetDescRec); the APD mirror of SQLBindParameter is the fallback when no
/// usable IPD record exists.
struct ParamSqlDescription {
  SQLSMALLINT sqlType = SQL_UNKNOWN_TYPE;
  SQLULEN columnSize = 0;
  SQLSMALLINT decimalDigits = 0;
};
ParamSqlDescription paramSqlDescription(const Statement& s, SQLSMALLINT index1, const DescRecord& apdRec) {
  ParamSqlDescription d;
  const DescRecord* ipd = s.ipd->recordIf(index1);
  if (ipd && isValidSqlType(ipd->conciseType) && ipd->conciseType != SQL_UNKNOWN_TYPE) {
    d.sqlType = ipd->conciseType;
    d.columnSize = ipd->boundColumnSize;
    d.decimalDigits = ipd->boundDecimalDigits;
  } else {
    d.sqlType = apdRec.boundSqlType;
    d.columnSize = apdRec.boundColumnSize;
    d.decimalDigits = apdRec.boundDecimalDigits;
  }
  return d;
}

/// Effective C type of parameter `index1` (SQL_C_DEFAULT follows the parameter's SQL type).
SQLSMALLINT effectiveParamCType(const Statement& s, SQLSMALLINT index1, const DescRecord& rec) {
  SQLSMALLINT c = normalizeCType(rec.conciseType);
  if (c == SQL_C_DEFAULT) {
    c = normalizeCType(defaultCTypeFor(paramSqlDescription(s, index1, rec).sqlType));
    if (c == SQL_C_DEFAULT) c = SQL_C_CHAR;
  }
  return c;
}

/// Locks the session to read the current transaction id.
std::string sessionTransactionId(const Connection& c) {
  if (!c.session) return {};
  std::lock_guard<std::mutex> lock(c.session->mutex);
  return c.session->transactionId;
}

/// ISOLATION LEVEL clause of START TRANSACTION for SQL_ATTR_TXN_ISOLATION.
const char* isolationLevelSql(SQLUINTEGER level) {
  switch (level) {
    case SQL_TXN_READ_COMMITTED: return "READ COMMITTED";
    case SQL_TXN_REPEATABLE_READ: return "REPEATABLE READ";
    case SQL_TXN_SERIALIZABLE: return "SERIALIZABLE";
    default: return "READ UNCOMMITTED";
  }
}

/// Opens the driver-managed transaction. In Trino `X-Trino-Transaction-Id: NONE` on an ordinary
/// statement only says "the client supports transactions and has none open": the statement then
/// runs in its own autocommit transaction. A transaction exists only once START TRANSACTION
/// returned X-Trino-Started-Transaction-Id; later statements (and COMMIT/ROLLBACK from
/// SQLEndTran) carry that id, which the client takes from the session.
void beginDriverTransaction(Statement& s, Connection& conn) {
  ExecOptions opts;
  opts.internal = true;
  opts.startTransaction = true;  // sends X-Trino-Transaction-Id: NONE, records the started id
  opts.cancelFlag = &s.cancelRequested;
  opts.queryTimeoutSec = static_cast<int>(std::min<SQLULEN>(s.queryTimeout, 0x7fffffff));
  std::string sql = std::string("START TRANSACTION ISOLATION LEVEL ") + isolationLevelSql(conn.txnIsolation);
  if (conn.accessMode == SQL_MODE_READ_ONLY) sql += ", READ ONLY";
  TRINO_LOG_DEBUG(kLog, "%s", sql.c_str());
  conn.client->executeToMemory(sql, opts);
  if (sessionTransactionId(conn).empty()) {
    throw DriverError(sqlstate::kGeneralError, "START TRANSACTION did not return a transaction id (X-Trino-Started-Transaction-Id)");
  }
  conn.inTransaction = true;
}

/// Adds Trino query warnings as 01000 records carrying the server prefix.
SQLRETURN addQueryWarnings(Diag& diag, const std::vector<QueryWarning>& warnings) {
  SQLRETURN rc = SQL_SUCCESS;
  for (const QueryWarning& w : warnings) {
    DiagRecord rec;
    rec.sqlstate = sqlstate::kGeneralWarning;
    rec.native = w.code;
    rec.message = Diag::formatServerMessage(w.name.empty() ? w.message : w.name + ": " + w.message);
    rc = mergeReturnCode(rc, diag.add(std::move(rec)));
  }
  return rc;
}

/// Column index by (case-insensitive) name in a metadata result; -1 when absent.
int columnIndex(const std::vector<ResultColumn>& cols, std::string_view name) {
  for (size_t i = 0; i < cols.size(); ++i) {
    if (iequals(cols[i].name, name)) return static_cast<int>(i);
  }
  return -1;
}

std::string cellText(const std::vector<std::optional<std::string>>& row, int index) {
  if (index < 0 || static_cast<size_t>(index) >= row.size() || !row[static_cast<size_t>(index)]) return {};
  return *row[static_cast<size_t>(index)];
}

/// Builds the per-set ParamValue list from the APD/IPD records. Sets *needData when a
/// data-at-execution parameter has not received its data yet (the caller enters NeedData).
std::vector<ParamValue> buildParamValues(Statement& s, SQLULEN set, bool* needData) {
  std::vector<ParamValue> values;
  values.reserve(s.paramCount);
  *needData = false;
  const Descriptor& apd = *s.apd;
  const SQLINTEGER bindType = apd.header.bindType;
  const SQLLEN offset = apd.header.bindOffsetPtr ? *apd.header.bindOffsetPtr : 0;

  if (s.dae.buffers.size() < s.paramCount) s.dae.buffers.resize(s.paramCount);
  if (s.dae.provided.size() < s.paramCount) s.dae.provided.resize(s.paramCount, false);
  if (s.dae.nulls.size() < s.paramCount) s.dae.nulls.resize(s.paramCount, false);

  for (size_t i = 0; i < s.paramCount; ++i) {
    const SQLSMALLINT index1 = static_cast<SQLSMALLINT>(i + 1);
    const DescRecord* rec = apd.recordIf(index1);
    if (!rec || !rec->bound) {
      throw DriverError(sqlstate::kWrongNumberOfParams,
                        formatString("The statement has %zu parameter marker(s) but parameter %d is not bound",
                                     s.paramCount, static_cast<int>(index1)));
    }
    const SQLSMALLINT cType = effectiveParamCType(s, index1, *rec);
    const size_t fixedSize = cTypeSize(cType);
    const size_t elementSize = fixedSize ? fixedSize : static_cast<size_t>(rec->octetLength < 0 ? 0 : rec->octetLength);

    ParamValue v;
    v.cType = rec->conciseType;
    const ParamSqlDescription described = paramSqlDescription(s, index1, *rec);
    v.sqlType = described.sqlType;
    v.columnSize = described.columnSize;
    v.decimalDigits = described.decimalDigits;
    v.bufferLength = rec->octetLength;
    v.data = elementAddress(rec->dataPtr, bindType, offset, set, elementSize);
    SQLLEN* lenPtr = rec->indicatorPtr ? rec->indicatorPtr : rec->octetLengthPtr;
    if (lenPtr) {
      lenPtr = static_cast<SQLLEN*>(elementAddress(lenPtr, bindType, offset, set, sizeof(SQLLEN)));
      v.length = *lenPtr;
    } else {
      v.length = SQL_NTS;  // no indicator: non-NULL, character data is NUL-terminated (ODBC default)
    }

    if (v.length == SQL_NULL_DATA) {
      v.isNull = true;
    } else if (v.length == SQL_DEFAULT_PARAM) {
      v.isDefault = true;
    } else if (isDataAtExecLength(v.length)) {
      if (s.dae.nulls[i]) {
        v.isNull = true;
      } else if (s.dae.provided[i]) {
        v.daeData = &s.dae.buffers[i];
      } else {
        *needData = true;
      }
    }
    values.push_back(v);
  }
  return values;
}

/// Type from DESCRIBE INPUT for parameter `index0` when it is known and not "unknown".
std::optional<TrinoType> describedParamType(const Statement& s, size_t index0) {
  if (!s.describedInputValid) return std::nullopt;
  for (const ParamInfo& p : s.describedParams) {
    if (p.position == static_cast<int>(index0 + 1)) {
      if (p.type.category == TypeCategory::Unknown) return std::nullopt;
      return p.type;
    }
  }
  return std::nullopt;
}

/// Text sent to Trino for one parameter set (plain SQL, EXECUTE IMMEDIATE ... USING, or EXECUTE name USING).
std::string buildExecutionSql(Statement& s, const std::vector<ParamValue>& values, ExecOptions& opts) {
  if (s.paramCount == 0) return s.sql;
  const ConvertOptions copts = s.convertOptions();
  std::string literals;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) literals += ", ";
    try {
      literals += renderParameterLiteral(values[i], copts, describedParamType(s, i));
    } catch (DriverError& e) {
      // Conversion failures name the offending parameter so SQLGetDiagField(SQL_DIAG_COLUMN_NUMBER) is useful.
      e.atColumn(static_cast<SQLINTEGER>(i + 1));
      throw;
    }
  }
  if (s.conn->config.explicitPrepare) {
    opts.preparedStatements[s.prepName] = s.sql;
    return "EXECUTE " + s.prepName + " USING " + literals;
  }
  // EXECUTE IMMEDIATE takes the statement as a string literal: single quotes are doubled by sqlQuoteLiteral.
  return "EXECUTE IMMEDIATE " + sqlQuoteLiteral(s.sql) + " USING " + literals;
}

/// Runs one parameter set to completion (or until the cursor is open). Returns the call's
/// return code (warnings -> SQL_SUCCESS_WITH_INFO). Throws on failure.
SQLRETURN runParameterSet(Statement& s, const std::vector<ParamValue>& values, const SqlKind& kind, SQLULEN set,
                          SQLULEN setCount) {
  Connection& conn = *s.conn;
  ExecOptions opts;
  opts.queryTimeoutSec = static_cast<int>(std::min<SQLULEN>(s.queryTimeout, 0x7fffffff));
  opts.maxRows = static_cast<int64_t>(s.maxRows);
  opts.cancelFlag = &s.cancelRequested;
  opts.activity = s.callDepth.activity();  // IdleFetchTimeout watches this statement's calls
  const std::string sqlToSend = buildExecutionSql(s, values, opts);

  // Driver-managed transactions: the first statement after autocommit was switched off opens one
  // (START TRANSACTION round trip); the statement itself then runs inside it.
  const bool startTransaction = conn.config.enableTransactions && conn.autocommit == SQL_AUTOCOMMIT_OFF &&
                                sessionTransactionId(conn).empty() && !kind.isTransaction;

  TRINO_LOG_DEBUG(kLog, "execute set %llu/%llu: %.*s%s", static_cast<unsigned long long>(set + 1),
                  static_cast<unsigned long long>(setCount), static_cast<int>(std::min<size_t>(sqlToSend.size(), 500)),
                  sqlToSend.c_str(), sqlToSend.size() > 500 ? "..." : "");

  std::shared_ptr<QueryExecution> query;
  {
    s.state = StmtState::Executing;
    if (startTransaction) beginDriverTransaction(s, conn);
    std::unique_ptr<QueryExecution> q = conn.client->execute(sqlToSend, opts);
    if (!q) throw DriverError(sqlstate::kGeneralError, "The client returned no query execution");
    query = std::shared_ptr<QueryExecution>(std::move(q));
  }
  s.queryId = query->queryId();
  s.setActiveQuery(query);

  // An application-issued COMMIT / ROLLBACK (or START TRANSACTION) changes the session's id.
  if (kind.isTransaction) conn.inTransaction = !sessionTransactionId(conn).empty();

  SQLRETURN rc = addQueryWarnings(s.diag, query->warnings());

  if (query->hasResultSet()) {
    if (setCount > 1) {
      // SQL_PARAM_ARRAY_SELECTS = SQL_PAS_NO_SELECT: result-set statements take one parameter set.
      query->cancel();
      query->close();
      s.setActiveQuery(nullptr);
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                        "Parameter arrays are not supported for statements that return a result set");
    }
    s.result = std::make_unique<ResultSet>(std::make_unique<SharedQueryRowSource>(query));
    s.result->setMaxRows(static_cast<int64_t>(s.maxRows));
    s.rebuildIrd(s.result->columns());
    s.getData.reset(s.result->columnCount());
    s.rowsetPos = 1;
    s.rowNumber = 0;
    s.rowCount = -1;
    s.diag.rowCount = -1;
    s.state = StmtState::CursorOpen;
    TRINO_LOG_DEBUG(kLog, "query %s opened a cursor with %zu column(s)", s.queryId.c_str(), s.result->columnCount());
    return rc;
  }

  // DML / DDL / session statements: the query already ran to completion inside execute().
  const std::optional<int64_t> updated = query->updateCount();
  const SQLLEN thisCount = updated ? static_cast<SQLLEN>(*updated) : (kind.isDml ? 0 : -1);
  if (set == 0 || s.rowCount < 0) {
    s.rowCount = thisCount;
  } else if (thisCount >= 0) {
    s.rowCount += thisCount;
  }
  query->close();
  s.setActiveQuery(nullptr);
  s.result.reset();
  s.getData.offsets.clear();
  s.rowsetPos = 1;
  s.rowNumber = 0;
  s.diag.rowCount = s.rowCount;
  s.state = StmtState::ExecutedNoCursor;
  TRINO_LOG_DEBUG(kLog, "query %s finished without a cursor, rows=%lld (%s)", s.queryId.c_str(),
                  static_cast<long long>(s.rowCount), query->updateType().c_str());
  return rc;
}

/// Splits DESCRIBE OUTPUT / DESCRIBE INPUT results into text rows (null cells -> empty text).
std::vector<std::vector<std::optional<std::string>>> drainRows(MemoryRowSource& rows) {
  std::vector<std::vector<std::optional<std::string>>> out;
  while (std::unique_ptr<Page> page = rows.nextPage()) {
    std::vector<std::vector<std::optional<std::string>>> chunk = pageToStrings(*page);
    out.insert(out.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
  }
  return out;
}

/// Runs an internal DESCRIBE statement with the statement's SQL registered as a prepared statement.
std::unique_ptr<MemoryRowSource> runDescribe(Statement& s, const char* what) {
  requireConnected(*s.conn);
  if (s.sql.empty() || s.prepName.empty()) {
    throw DriverError(sqlstate::kFunctionSequence, "No SQL statement has been prepared");
  }
  ExecOptions opts;
  opts.internal = true;
  opts.preparedStatements[s.prepName] = s.sql;
  opts.cancelFlag = &s.cancelRequested;
  const std::string sql = std::string("DESCRIBE ") + what + " " + s.prepName;
  TRINO_LOG_DEBUG(kLog, "%s", sql.c_str());
  std::unique_ptr<MemoryRowSource> rows = s.conn->client->executeToMemory(sql, opts);
  if (!rows) throw DriverError(sqlstate::kGeneralError, sql + " produced no result");
  return rows;
}

#ifdef SQL_SETPARAM_VALUE_MAX
constexpr SQLLEN kSetParamValueMax = SQL_SETPARAM_VALUE_MAX;
#else
constexpr SQLLEN kSetParamValueMax = -1;
#endif

/// Validates SQLBindParameter arguments and writes the APD/IPD records (shared with SQLSetParam).
SQLRETURN bindParameter(Statement& s, SQLUSMALLINT param, SQLSMALLINT ioType, SQLSMALLINT cType, SQLSMALLINT sqlType,
                        SQLULEN columnSize, SQLSMALLINT decimalDigits, SQLPOINTER value, SQLLEN bufferLength,
                        SQLLEN* strLenOrInd) {
  requireNotBusy(s);
  if (param == 0) throw DriverError(sqlstate::kInvalidParameterNumber, "Parameter number must be greater than 0");
  if (param > 0x7fff) {
    // Descriptor records are addressed with SQLSMALLINT: larger numbers can never be a parameter.
    throw DriverError(sqlstate::kInvalidDescriptorIndex,
                      "Parameter number " + std::to_string(static_cast<unsigned>(param)) + " is out of range");
  }
  // Driver managers map ODBC 2 SQLSetParam onto SQLBindParameter(SQL_PARAM_INPUT_OUTPUT, ...,
  // BufferLength = SQL_SETPARAM_VALUE_MAX) and ISO SQLBindParam onto (SQL_PARAM_INPUT, ...,
  // SQL_SETPARAM_VALUE_MAX) ("SQLSetParam Mapping"). The sentinel means "buffer length unknown";
  // those parameters are input parameters in everything but name.
  const bool setParamMapping = bufferLength == kSetParamValueMax;
  if (setParamMapping) {
    bufferLength = 0;
    if (ioType == SQL_PARAM_INPUT_OUTPUT) ioType = SQL_PARAM_INPUT;
  }
  switch (ioType) {
    case SQL_PARAM_INPUT:
      break;
    case SQL_PARAM_INPUT_OUTPUT:
    case SQL_PARAM_OUTPUT:
#ifdef SQL_PARAM_INPUT_OUTPUT_STREAM
    case SQL_PARAM_INPUT_OUTPUT_STREAM:
    case SQL_PARAM_OUTPUT_STREAM:
#endif
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented, "Only input parameters are supported");
    default:
      throw DriverError(sqlstate::kInvalidParameterType,
                        "Invalid parameter type " + std::to_string(static_cast<int>(ioType)));
  }
  if (!isValidCType(cType)) {
    throw DriverError(sqlstate::kInvalidAppBufferType,
                      "Invalid application buffer type " + std::to_string(static_cast<int>(cType)));
  }
  if (!isValidSqlType(sqlType)) {
    throw DriverError(sqlstate::kInvalidSqlDataType, "Invalid SQL data type " + std::to_string(static_cast<int>(sqlType)));
  }
  if (bufferLength < 0) {
    throw DriverError(sqlstate::kInvalidStringOrBufferLength,
                      "BufferLength must not be negative (" + std::to_string(static_cast<long long>(bufferLength)) + ")");
  }
  if (!value && strLenOrInd && !isDataAtExecLength(*strLenOrInd) && *strLenOrInd != SQL_NULL_DATA &&
      *strLenOrInd != SQL_DEFAULT_PARAM) {
    // A null value pointer is only meaningful for NULL, DEFAULT or data-at-execution parameters
    // (the indicator is read again at execute time; this catches the obvious misuse early).
    throw DriverError(sqlstate::kInvalidNullPointer, "ParameterValuePtr is null and the indicator is not NULL/DAE");
  }

  const SQLSMALLINT normalizedC = normalizeCType(cType);
  DescRecord& a = s.apd->record(static_cast<SQLSMALLINT>(param));
  a.conciseType = cType;
  a.type = normalizedC == SQL_C_DEFAULT ? SQL_C_DEFAULT : verboseTypeOf(normalizedC);
  a.datetimeIntervalCode = normalizedC == SQL_C_DEFAULT ? 0 : datetimeSubcodeOf(normalizedC);
  a.octetLength = bufferLength;
  a.length = bufferLength;
  a.dataPtr = value;
  a.octetLengthPtr = strLenOrInd;
  a.indicatorPtr = strLenOrInd;
  a.bound = true;
  a.boundSqlType = sqlType;
  a.boundColumnSize = columnSize;
  a.boundDecimalDigits = decimalDigits;
  if (normalizedC == SQL_C_NUMERIC) {
    // ODBC: SQLBindParameter sets SQL_DESC_PRECISION/SCALE of the APD from ColumnSize/DecimalDigits.
    a.precision = static_cast<SQLSMALLINT>(std::min<SQLULEN>(columnSize, 0x7fff));
    a.scale = decimalDigits;
    a.precisionSet = columnSize != 0;
    a.scaleSet = true;
  }

  DescRecord& p = s.ipd->record(static_cast<SQLSMALLINT>(param));
  p.conciseType = sqlType;
  p.type = verboseTypeOf(sqlType);
  p.datetimeIntervalCode = datetimeSubcodeOf(sqlType);
  p.length = static_cast<SQLLEN>(columnSize);
  p.octetLength = static_cast<SQLLEN>(columnSize);
  p.precision = static_cast<SQLSMALLINT>(std::min<SQLULEN>(columnSize, 0x7fff));
  p.scale = decimalDigits;
  p.nullable = SQL_NULLABLE_UNKNOWN;
  p.parameterType = ioType;
  p.bound = true;
  p.autoPopulated = false;
  p.boundSqlType = sqlType;
  p.boundColumnSize = columnSize;
  p.boundDecimalDigits = decimalDigits;
  p.typeName = trinoTypeForSqlType(sqlType, columnSize, decimalDigits);
  TRINO_LOG_TRACE(kLog, "bind parameter %u: C=%s SQL=%s size=%llu digits=%d buf=%lld", static_cast<unsigned>(param),
                  cTypeName(cType), sqlTypeName(sqlType), static_cast<unsigned long long>(columnSize),
                  static_cast<int>(decimalDigits), static_cast<long long>(bufferLength));
  return SQL_SUCCESS;
}

/// Bytes SQLPutData contributes for `cType` given the application's StrLen_or_Ind.
size_t putDataByteCount(SQLSMALLINT cType, const void* data, SQLLEN strLenOrInd) {
  if (cType == SQL_C_CHAR) {
    if (strLenOrInd == SQL_NTS) return data ? std::strlen(static_cast<const char*>(data)) : 0;
  } else if (cType == SQL_C_WCHAR) {
    if (strLenOrInd == SQL_NTS) return data ? wideStrLen(static_cast<const SQLWCHAR*>(data)) * sizeof(SQLWCHAR) : 0;
  } else if (cType != SQL_C_BINARY) {
    return cTypeSize(cType);  // fixed-size values ignore StrLen_or_Ind
  }
  if (strLenOrInd < 0) {
    throw DriverError(sqlstate::kInvalidStringOrBufferLength,
                      "Invalid StrLen_or_Ind " + std::to_string(static_cast<long long>(strLenOrInd)) + " in SQLPutData");
  }
  return static_cast<size_t>(strLenOrInd);
}

}  // namespace

// ---- impl ---------------------------------------------------------------------------------------
namespace impl {

SQLRETURN prepare(Statement& s, std::string sql) {
  requireNotBusy(s);
  requireNoCursor(s);
  requireConnected(*s.conn);
  if (trim(sql).empty()) throw DriverError(sqlstate::kSyntaxError, "The SQL statement is empty");

  // A previous result without an open cursor (DML) is discarded; the cached metadata goes with it.
  s.closeCursor();
  s.resetPrepared();
  s.originalSql = std::move(sql);
  std::vector<std::string> warnings;
  s.sql = s.noscan == SQL_NOSCAN_ON ? s.originalSql : translateOdbcEscapes(s.originalSql, &warnings);
  s.translateWarnings = warnings;
  s.paramCount = countParameterMarkers(s.sql);
  s.prepName = "odbc_stmt_" + std::to_string(++g_prepCounter);
  s.prepared = true;
  s.state = StmtState::Prepared;
  s.rowCount = -1;
  Diag::classifyDynamicFunction(s.sql, s.diag.dynamicFunction, s.diag.dynamicFunctionCode);
  TRINO_LOG_DEBUG(kLog, "prepared %s (%zu parameter(s)): %.*s%s", s.prepName.c_str(), s.paramCount,
                  static_cast<int>(std::min<size_t>(s.sql.size(), 500)), s.sql.c_str(), s.sql.size() > 500 ? "..." : "");

  SQLRETURN rc = SQL_SUCCESS;
  for (const std::string& w : warnings) rc = mergeReturnCode(rc, s.diag.addWarning(sqlstate::kGeneralWarning, w));
  return rc;
}

SQLRETURN execDirect(Statement& s, std::string sql) {
  const SQLRETURN prepRc = prepare(s, std::move(sql));
  if (prepRc == SQL_ERROR) return prepRc;
  // SQLExecDirect leaves no prepared statement behind: closing the cursor returns to Allocated.
  s.prepared = false;
  SQLRETURN rc;
  try {
    rc = executeStatement(s);
  } catch (...) {
    if (!s.result && s.state != StmtState::NeedData) s.state = StmtState::Allocated;
    throw;
  }
  if (rc == SQL_ERROR && !s.result && s.state != StmtState::NeedData) s.state = StmtState::Allocated;
  return mergeReturnCode(prepRc, rc);
}

SQLRETURN executeStatement(Statement& s) {
  requireConnected(*s.conn);
  if (s.sql.empty()) throw DriverError(sqlstate::kFunctionSequence, "No SQL statement has been prepared");
  Diag::classifyDynamicFunction(s.sql, s.diag.dynamicFunction, s.diag.dynamicFunctionCode);

  Connection& conn = *s.conn;
  const bool resuming = s.dae.active;
  StateGuard guard{s, idleState(s)};

  if (!resuming) {
    // Fresh execution: discard leftovers of the previous one (a DML result, an exhausted cursor).
    if (s.result) {
      s.result->close();
      s.result.reset();
    }
    s.setActiveQuery(nullptr);
    s.getData.offsets.clear();
    s.rowsetPos = 1;
    s.rowNumber = 0;
    s.rowCount = -1;
    s.queryId.clear();
    s.currentParamSet = 0;
    s.dae = DaeState{};
    // cancelRequested is not reset here: guardStmt clears it when the call starts, and a SQLCancel
    // that arrives from now on must reach the query this call is about to run.
    if (s.describedOutputValid && !s.describedColumns.empty()) {
      s.ird->populateFromColumns(s.describedColumns);
    } else {
      s.ird->clearRecords();
    }
  }

  const SqlKind kind = classifySql(s.sql);
  const bool readOnly = conn.config.readOnly || conn.accessMode == SQL_MODE_READ_ONLY;
  if (readOnly && (kind.isDml || kind.isDdl)) {
    throw DriverError(sqlstate::kSyntaxError, "The connection is read-only; " + kind.firstKeyword + " statements are rejected");
  }
  if (s.paramCount > 0 && static_cast<size_t>(s.apd->count()) < s.paramCount) {
    throw DriverError(sqlstate::kWrongNumberOfParams,
                      formatString("The statement has %zu parameter marker(s) but only %d parameter(s) are bound",
                                   s.paramCount, static_cast<int>(s.apd->count())));
  }

  const SQLULEN setCount = s.paramCount == 0 ? 1 : std::max<SQLULEN>(s.apd->header.arraySize, 1);
  SQLULEN* processedPtr = s.ipd->header.rowsProcessedPtr;
  SQLUSMALLINT* statusPtr = s.ipd->header.arrayStatusPtr;
  const SQLUSMALLINT* operationPtr = s.apd->header.arrayStatusPtr;
  if (!resuming && processedPtr) *processedPtr = 0;
  if (!resuming && statusPtr) {
    for (SQLULEN k = 0; k < setCount; ++k) statusPtr[k] = SQL_PARAM_UNUSED;
  }

  SQLRETURN rc = SQL_SUCCESS;
  for (SQLULEN set = s.currentParamSet; set < setCount; ++set) {
    s.currentParamSet = set;
    if (operationPtr && operationPtr[set] == SQL_PARAM_IGNORE) {
      if (statusPtr) statusPtr[set] = SQL_PARAM_UNUSED;
      if (processedPtr) *processedPtr = set + 1;
      s.dae = DaeState{};
      continue;
    }
    bool needData = false;
    std::vector<ParamValue> values = buildParamValues(s, set, &needData);
    if (needData) {
      s.dae.active = true;
      s.dae.paramSet = set;
      s.dae.currentParam = 0;
      s.dae.putDataCalled = false;
      s.state = StmtState::NeedData;
      guard.committed = true;
      TRINO_LOG_DEBUG(kLog, "parameter set %llu needs data-at-execution input", static_cast<unsigned long long>(set));
      return SQL_NEED_DATA;
    }
    SQLRETURN setRc;
    try {
      setRc = runParameterSet(s, values, kind, set, setCount);
    } catch (DriverError& e) {
      if (statusPtr) statusPtr[set] = SQL_PARAM_ERROR;
      if (processedPtr) *processedPtr = set + 1;
      if (setCount > 1 && e.rowNumber() == SQL_NO_ROW_NUMBER) e.atRow(static_cast<SQLLEN>(set + 1));
      throw;
    }
    if (statusPtr) statusPtr[set] = setRc == SQL_SUCCESS_WITH_INFO ? SQL_PARAM_SUCCESS_WITH_INFO : SQL_PARAM_SUCCESS;
    if (processedPtr) *processedPtr = set + 1;
    rc = mergeReturnCode(rc, setRc);
    s.dae = DaeState{};  // the next set starts with fresh data-at-execution bookkeeping
  }
  s.currentParamSet = 0;
  guard.committed = true;
  return rc;
}

void ensureDescribedOutput(Statement& s) {
  if (s.describedOutputValid) return;
  std::unique_ptr<MemoryRowSource> rows = runDescribe(s, "OUTPUT");
  const std::vector<ResultColumn>& meta = rows->columns();
  const int iName = columnIndex(meta, "Column Name");
  const int iCatalog = columnIndex(meta, "Catalog");
  const int iSchema = columnIndex(meta, "Schema");
  const int iTable = columnIndex(meta, "Table");
  const int iType = columnIndex(meta, "Type");
  const int iAliased = columnIndex(meta, "Aliased");
  if (iType < 0) throw DriverError(sqlstate::kGeneralError, "DESCRIBE OUTPUT returned no Type column");

  const std::vector<std::vector<std::optional<std::string>>> data = drainRows(*rows);
  const TypeMappingOptions mapping = s.conn->client ? s.conn->client->typeMapping() : s.conn->config.typeMapping();
  const SqlKind kind = classifySql(s.sql);

  std::vector<ResultColumn> columns;
  columns.reserve(data.size());
  bool anyNamed = false;
  for (const auto& row : data) {
    const std::string name = cellText(row, iName);
    const std::string type = cellText(row, iType);
    if (!name.empty() || !iequals(type, "unknown")) anyNamed = true;
    ResultColumn col = ResultColumn::make(name, type.empty() ? std::string("unknown") : type, mapping);
    col.catalog = cellText(row, iCatalog);
    col.schema = cellText(row, iSchema);
    col.table = cellText(row, iTable);
    const std::string aliased = cellText(row, iAliased);
    col.aliased = iequals(aliased, "true") || aliased == "1";
    columns.push_back(std::move(col));
  }
  // Trino describes DML/DDL either with zero rows or with a nameless "unknown" placeholder; the
  // statement kind is the final word so an INSERT never looks like a one-column result set.
  const bool isResultSet = !data.empty() && anyNamed && !(kind.isDml || kind.isDdl || kind.isTransaction ||
                                                          kind.isUse || kind.isSet);
  s.describedColumns = isResultSet ? std::move(columns) : std::vector<ResultColumn>{};
  s.describedOutputIsResultSet = isResultSet;
  s.describedOutputValid = true;
  if (!s.result) s.ird->populateFromColumns(s.describedColumns);
  TRINO_LOG_DEBUG(kLog, "%s described: %zu column(s), resultSet=%d", s.prepName.c_str(), s.describedColumns.size(),
                  isResultSet ? 1 : 0);
}

void ensureDescribedInput(Statement& s) {
  if (s.describedInputValid) return;
  std::unique_ptr<MemoryRowSource> rows = runDescribe(s, "INPUT");
  const std::vector<ResultColumn>& meta = rows->columns();
  const int iPos = columnIndex(meta, "Position");
  const int iType = columnIndex(meta, "Type");
  if (iPos < 0 || iType < 0) throw DriverError(sqlstate::kGeneralError, "DESCRIBE INPUT returned an unexpected layout");

  std::vector<ParamInfo> params;
  for (const auto& row : drainRows(*rows)) {
    int64_t pos = 0;
    if (!parseInt64(cellText(row, iPos), &pos) || pos < 1) continue;
    ParamInfo p;
    p.position = static_cast<int>(pos);
    const std::string type = cellText(row, iType);
    p.type = TrinoType::parse(type.empty() ? std::string("unknown") : type);
    if (iequals(type, "unknown")) p.type.category = TypeCategory::Unknown;
    params.push_back(std::move(p));
  }
  std::sort(params.begin(), params.end(), [](const ParamInfo& a, const ParamInfo& b) { return a.position < b.position; });

  // Auto-populate IPD records the application has not bound itself (SQL_ATTR_ENABLE_AUTO_IPD).
  const TypeMappingOptions mapping = s.conn->client ? s.conn->client->typeMapping() : s.conn->config.typeMapping();
  for (const ParamInfo& p : params) {
    if (p.position > 0x7fff) continue;
    const DescRecord* existing = s.ipd->recordIf(static_cast<SQLSMALLINT>(p.position));
    // Records the application wrote (SQLBindParameter, SQLSetDescField/Rec on the IPD) win.
    if (existing && (existing->bound || (!existing->autoPopulated && existing->conciseType != SQL_C_DEFAULT))) continue;
    const OdbcTypeInfo info = mapTrinoType(p.type, mapping);
    DescRecord& r = s.ipd->record(static_cast<SQLSMALLINT>(p.position));
    r.conciseType = info.conciseType;
    r.type = info.verboseType;
    r.datetimeIntervalCode = info.intervalType ? info.intervalCode : info.datetimeSubcode;
    r.length = static_cast<SQLLEN>(info.columnSize);
    r.octetLength = info.octetLength;
    r.precision = info.precision;
    r.scale = info.decimalDigits;
    r.numPrecRadix = info.numPrecRadix;
    r.nullable = SQL_NULLABLE_UNKNOWN;
    r.parameterType = SQL_PARAM_INPUT;
    r.typeName = info.typeName;
    r.unnamed = SQL_UNNAMED;
    r.autoPopulated = true;  // describes this statement only: dropped again by the next prepare
  }
  s.describedParams = std::move(params);
  s.describedInputValid = true;
  TRINO_LOG_DEBUG(kLog, "%s input described: %zu parameter(s)", s.prepName.c_str(), s.describedParams.size());
}

SQLRETURN freeStatementHandle(SQLHSTMT h) {
  Statement* s = asStatement(h);
  if (!s) return SQL_INVALID_HANDLE;
  Connection* c = s->conn;
  if (!c) return SQL_INVALID_HANDLE;
  // Lock order: connection, then statement. Statement calls never take the connection mutex, so a
  // call blocked in SQLFetch/SQLExecute does not stop us here; destroyStatement cancels that call's
  // query before it waits for the statement mutex.
  HandleLock connLock(c->mutex);
  if (asStatement(h) != s) return SQL_INVALID_HANDLE;  // freed by another thread while we waited
  diag_cursor::reset(s->diag);
  diag_cursor::reset(s->implicitArd.diag);
  diag_cursor::reset(s->implicitApd.diag);
  diag_cursor::reset(s->implicitIrd.diag);
  diag_cursor::reset(s->implicitIpd.diag);
  c->destroyStatement(s);
  TRINO_LOG_TRACE(kLog, "SQLFreeStmt(SQL_DROP) -> 0");
  return SQL_SUCCESS;
}

SQLRETURN nativeSql(Connection& c, std::string in, TextOut& out) {
  std::vector<std::string> warnings;
  const std::string translated = translateOdbcEscapes(in, &warnings);
  SQLRETURN rc = out.writeWithDiag(translated, c.diag);
  for (const std::string& w : warnings) rc = mergeReturnCode(rc, c.diag.addWarning(sqlstate::kGeneralWarning, w));
  return rc;
}

SQLRETURN getCursorName(Statement& s, TextOut& name) {
  requireNotBusy(s);
  return name.writeWithDiag(s.cursorName, s.diag);
}

SQLRETURN setCursorName(Statement& s, std::string name) {
  requireNotBusy(s);
  if (s.state == StmtState::CursorOpen || s.state == StmtState::Fetched) {
    throw DriverError(sqlstate::kInvalidCursorState, "Cannot set the cursor name while a cursor is open");
  }
  const std::string trimmed(trim(name));
  if (trimmed.empty() || utf8CodePoints(trimmed) > 128) {
    throw DriverError(sqlstate::kInvalidCursorName, "Cursor names must be 1 to 128 characters long");
  }
  // SQLCUR / SQL_CUR prefixes are reserved for driver-generated names (34000).
  const std::string upper = toUpper(trimmed);
  if (upper.rfind("SQLCUR", 0) == 0 || upper.rfind("SQL_CUR", 0) == 0) {
    throw DriverError(sqlstate::kInvalidCursorName,
                      "Invalid cursor name '" + trimmed + "': the prefixes SQLCUR and SQL_CUR are reserved");
  }
  // Names are unique per connection (3C000), default names included; the statement's own name may
  // be re-set freely.
  for (const Statement* other : s.conn->statements) {
    if (other != &s && iequals(other->cursorName, trimmed)) {
      throw DriverError(sqlstate::kDuplicateCursorName, "Cursor name '" + trimmed + "' is already in use");
    }
  }
  s.cursorName = trimmed;
  s.cursorNameExplicit = true;
  return SQL_SUCCESS;
}

}  // namespace impl

}  // namespace trino_odbc

// ---- exported entry points ---------------------------------------------------------------------
using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLPrepare(SQLHSTMT hstmt, SQLCHAR* szSqlStr, SQLINTEGER cbSqlStr) {
  return guardStmt(hstmt, "SQLPrepare", [&](Statement& s) -> SQLRETURN {
    std::optional<std::string> sql = textIn(szSqlStr, cbSqlStr, ansiEncodingOf(s));
    if (!sql) throw DriverError(sqlstate::kInvalidNullPointer, "StatementText is null");
    return impl::prepare(s, std::move(*sql));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLExecDirect(SQLHSTMT hstmt, SQLCHAR* szSqlStr, SQLINTEGER cbSqlStr) {
  return guardStmt(hstmt, "SQLExecDirect", [&](Statement& s) -> SQLRETURN {
    std::optional<std::string> sql = textIn(szSqlStr, cbSqlStr, ansiEncodingOf(s));
    if (!sql) throw DriverError(sqlstate::kInvalidNullPointer, "StatementText is null");
    return impl::execDirect(s, std::move(*sql));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLExecute(SQLHSTMT hstmt) {
  return guardStmt(hstmt, "SQLExecute", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::CursorOpen || s.state == StmtState::Fetched) {
      throw DriverError(sqlstate::kInvalidCursorState, "A cursor is open; close it before executing again");
    }
    if (!s.prepared || s.state == StmtState::Allocated) {
      throw DriverError(sqlstate::kFunctionSequence, "SQLExecute requires a statement prepared with SQLPrepare");
    }
    return impl::executeStatement(s);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLNumParams(SQLHSTMT hstmt, SQLSMALLINT* pcpar) {
  return guardStmt(hstmt, "SQLNumParams", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::Allocated) throw DriverError(sqlstate::kFunctionSequence, "No statement has been prepared");
    if (pcpar) *pcpar = static_cast<SQLSMALLINT>(std::min<size_t>(s.paramCount, 0x7fff));
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT hstmt, SQLUSMALLINT ipar, SQLSMALLINT* pfSqlType,
                                                  SQLULEN* pcbParamDef, SQLSMALLINT* pibScale, SQLSMALLINT* pfNullable) {
  return guardStmt(hstmt, "SQLDescribeParam", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::Allocated) throw DriverError(sqlstate::kFunctionSequence, "No statement has been prepared");
    if (ipar == 0 || static_cast<size_t>(ipar) > s.paramCount) {
      throw DriverError(sqlstate::kInvalidDescriptorIndex,
                        formatString("Parameter number %u is out of range (1..%zu)", static_cast<unsigned>(ipar), s.paramCount));
    }
    impl::ensureDescribedInput(s);
    const TypeMappingOptions mapping = s.conn->client ? s.conn->client->typeMapping() : s.conn->config.typeMapping();
    const ParamInfo* info = nullptr;
    for (const ParamInfo& p : s.describedParams) {
      if (p.position == static_cast<int>(ipar)) info = &p;
    }
    SQLSMALLINT sqlType;
    SQLULEN size;
    SQLSMALLINT digits;
    if (!info || info->type.category == TypeCategory::Unknown) {
      // Trino cannot infer the type: describe it as text so any bound C type still renders.
      sqlType = mapping.unicode ? SQL_WVARCHAR : SQL_VARCHAR;
      size = static_cast<SQLULEN>(mapping.defaultStringLength);
      digits = 0;
    } else {
      const OdbcTypeInfo t = mapTrinoType(info->type, mapping);
      sqlType = t.conciseType;
      size = t.columnSize;
      digits = t.decimalDigits;
    }
    if (pfSqlType) *pfSqlType = sqlType;
    if (pcbParamDef) *pcbParamDef = size;
    if (pibScale) *pibScale = digits;
    if (pfNullable) *pfNullable = SQL_NULLABLE_UNKNOWN;
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLBindParameter(SQLHSTMT hstmt, SQLUSMALLINT ipar, SQLSMALLINT fParamType,
                                                  SQLSMALLINT fCType, SQLSMALLINT fSqlType, SQLULEN cbColDef,
                                                  SQLSMALLINT ibScale, SQLPOINTER rgbValue, SQLLEN cbValueMax,
                                                  SQLLEN* pcbValue) {
  return guardStmt(hstmt, "SQLBindParameter", [&](Statement& s) -> SQLRETURN {
    return bindParameter(s, ipar, fParamType, fCType, fSqlType, cbColDef, ibScale, rgbValue, cbValueMax, pcbValue);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetParam(SQLHSTMT hstmt, SQLUSMALLINT ipar, SQLSMALLINT fCType, SQLSMALLINT fSqlType,
                                             SQLULEN cbParamDef, SQLSMALLINT ibScale, SQLPOINTER rgbValue,
                                             SQLLEN* pcbValue) {
  return guardStmt(hstmt, "SQLSetParam", [&](Statement& s) -> SQLRETURN {
    // ODBC 2 SQLSetParam is SQLBindParameter without a buffer length (SQL_SETPARAM_VALUE_MAX) and
    // with input/output semantics we treat as input, the only direction Trino supports.
    return bindParameter(s, ipar, SQL_PARAM_INPUT, fCType, fSqlType, cbParamDef, ibScale, rgbValue, 0, pcbValue);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLParamOptions(SQLHSTMT hstmt, SQLULEN crow, SQLULEN* pirow) {
  return guardStmt(hstmt, "SQLParamOptions", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (crow == 0) throw DriverError(sqlstate::kInvalidAttributeValue, "The parameter array size must be at least 1");
    s.apd->header.arraySize = crow;
    s.ipd->header.rowsProcessedPtr = pirow;
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLParamData(SQLHSTMT hstmt, SQLPOINTER* prgbValue) {
  return guardStmt(hstmt, "SQLParamData", [&](Statement& s) -> SQLRETURN {
    if (s.state != StmtState::NeedData || !s.dae.active) {
      throw DriverError(sqlstate::kFunctionSequence, "SQLParamData is only valid after SQL_NEED_DATA");
    }
    for (;;) {
      // The parameter the application was feeding is complete (zero SQLPutData calls = empty value).
      if (s.dae.currentParam > 0) {
        const size_t idx = static_cast<size_t>(s.dae.currentParam - 1);
        if (idx < s.dae.provided.size()) s.dae.provided[idx] = true;
        s.dae.currentParam = 0;
        s.dae.putDataCalled = false;
      }
      // Hand out the next data-at-execution parameter of the current set that has no data yet.
      const SQLINTEGER bindType = s.apd->header.bindType;
      const SQLLEN offset = s.apd->header.bindOffsetPtr ? *s.apd->header.bindOffsetPtr : 0;
      for (size_t i = 0; i < s.paramCount; ++i) {
        const DescRecord* rec = s.apd->recordIf(static_cast<SQLSMALLINT>(i + 1));
        if (!rec || !rec->bound) continue;
        SQLLEN* lenPtr = rec->indicatorPtr ? rec->indicatorPtr : rec->octetLengthPtr;
        if (!lenPtr) continue;
        lenPtr = static_cast<SQLLEN*>(elementAddress(lenPtr, bindType, offset, s.dae.paramSet, sizeof(SQLLEN)));
        if (!isDataAtExecLength(*lenPtr)) continue;
        if (i < s.dae.provided.size() && (s.dae.provided[i] || s.dae.nulls[i])) continue;
        const SQLSMALLINT cType = effectiveParamCType(s, static_cast<SQLSMALLINT>(i + 1), *rec);
        const size_t fixed = cTypeSize(cType);
        const size_t elementSize = fixed ? fixed : static_cast<size_t>(rec->octetLength < 0 ? 0 : rec->octetLength);
        s.dae.currentParam = static_cast<SQLSMALLINT>(i + 1);
        s.dae.putDataCalled = false;
        if (i < s.dae.buffers.size()) s.dae.buffers[i].clear();
        if (prgbValue) *prgbValue = elementAddress(rec->dataPtr, bindType, offset, s.dae.paramSet, elementSize);
        TRINO_LOG_TRACE(kLog, "SQLParamData: parameter %zu awaits data", i + 1);
        return SQL_NEED_DATA;
      }
      // Every parameter of this set has its data: continue the execution. It may return
      // SQL_NEED_DATA again for a later parameter set, in which case the loop picks the next token.
      s.state = StmtState::Prepared;  // executeStatement re-enters NeedData itself when required
      const SQLRETURN rc = impl::executeStatement(s);
      if (rc != SQL_NEED_DATA) return rc;
    }
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLPutData(SQLHSTMT hstmt, SQLPOINTER rgbValue, SQLLEN cbValue) {
  return guardStmt(hstmt, "SQLPutData", [&](Statement& s) -> SQLRETURN {
    if (s.state != StmtState::NeedData || !s.dae.active || s.dae.currentParam == 0) {
      throw DriverError(sqlstate::kFunctionSequence, "SQLPutData requires a preceding SQLParamData that returned SQL_NEED_DATA");
    }
    const size_t idx = static_cast<size_t>(s.dae.currentParam - 1);
    const DescRecord* rec = s.apd->recordIf(s.dae.currentParam);
    if (!rec || idx >= s.dae.buffers.size()) {
      throw DriverError(sqlstate::kFunctionSequence, "Data-at-execution parameter is no longer bound");
    }
    if (cbValue == SQL_NULL_DATA) {
      if (s.dae.putDataCalled) {
        throw DriverError("HY020", "Attempt to concatenate a null value to data already sent for the parameter");
      }
      s.dae.nulls[idx] = true;
      s.dae.provided[idx] = true;
      s.dae.putDataCalled = true;
      return SQL_SUCCESS;
    }
    if (cbValue == SQL_DEFAULT_PARAM || isDataAtExecLength(cbValue)) {
      throw DriverError(sqlstate::kInvalidStringOrBufferLength,
                        "Invalid StrLen_or_Ind " + std::to_string(static_cast<long long>(cbValue)) + " in SQLPutData");
    }
    if (s.dae.nulls[idx]) {
      throw DriverError("HY020", "Attempt to concatenate data to a parameter already sent as NULL");
    }
    const SQLSMALLINT cType = effectiveParamCType(s, s.dae.currentParam, *rec);
    if (!isChunkedCType(cType) && s.dae.putDataCalled) {
      throw DriverError(sqlstate::kNonCharSentInPieces, "Non-character, non-binary data cannot be sent in pieces");
    }
    const size_t bytes = putDataByteCount(cType, rgbValue, cbValue);
    if (bytes > 0 && !rgbValue) throw DriverError(sqlstate::kInvalidNullPointer, "DataPtr is null");
    if (bytes > 0) s.dae.buffers[idx].append(static_cast<const char*>(rgbValue), bytes);
    s.dae.putDataCalled = true;
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLCancel(SQLHSTMT hstmt) {
  // Deliberately no guardStmt: SQLCancel is called from another thread while an API call holds the
  // statement mutex, so it must work without acquiring it.
  Statement* s = asStatement(hstmt);
  if (!s) return SQL_INVALID_HANDLE;
  if (s->callDepth.load() > 0) {
    // A function is in progress (on another thread): flag it (polled while execute() runs, before
    // a query object exists) and abort the transfer of the query it is working on.
    cancelStatementInProgress(*s);
    TRINO_LOG_DEBUG(kLog, "SQLCancel requested for in-flight call on statement %p", static_cast<void*>(s));
    return SQL_SUCCESS;
  }
  // No function is in progress. Applications such as Power BI cancel a refresh by calling
  // SQLCancel between two SQLFetch calls (and then free the handles, or simply stop calling), so a
  // query that is still running on the server is cancelled there right away (DELETE nextUri) and
  // the next fetch on the cursor reports HY008. Beyond that the ODBC 3.x rule holds: with nothing
  // running (no cursor, or a result the server has finished delivering) the call has no effect.
  if (cancelRunningQuery(*s)) {
    TRINO_LOG_DEBUG(kLog, "SQLCancel cancelled the running query of idle statement %p", static_cast<void*>(s));
  }
  // It also ends a data-at-execution sequence. The lock is only tried: another thread holding it
  // without a function in progress (a descriptor call, a statement being freed) is not something
  // to cancel.
  std::unique_lock<std::recursive_mutex> lock(s->mutex, std::try_to_lock);
  if (lock.owns_lock() && s->state == StmtState::NeedData) {
    s->dae = DaeState{};
    s->currentParamSet = 0;
    s->state = idleState(*s);
    TRINO_LOG_DEBUG(kLog, "SQLCancel abandoned data-at-execution");
  }
  return SQL_SUCCESS;
}

TRINO_ODBC_API SQLRETURN SQL_API SQLCancelHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  if (handleType == SQL_HANDLE_STMT) return SQLCancel(static_cast<SQLHSTMT>(handle));
  if (handleType == SQL_HANDLE_DBC) {
    Connection* c = asConnection(handle);
    if (!c) return SQL_INVALID_HANDLE;
    // The driver runs no connection function asynchronously, so there is no connection-level work
    // to cancel. While a connection function runs on another thread (SQLDisconnect,
    // SQLDriverConnect ...) the connection - its client and statement list - may be replaced or
    // freed under us, so nothing is touched without the connection lock.
    std::unique_lock<std::recursive_mutex> lock(c->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return SQL_SUCCESS;
    // Every statement is cancelled the way SQLCancel cancels it: the function in progress, or else
    // a query still running on the server behind an idle cursor. Idle statements keep no stale
    // cancel request, and results the server has finished delivering stay readable.
    for (Statement* s : c->statements) {
      if (s->callDepth.load() > 0) cancelStatementInProgress(*s);
      else cancelRunningQuery(*s);
    }
    return SQL_SUCCESS;
  }
  return SQL_INVALID_HANDLE;
}

TRINO_ODBC_API SQLRETURN SQL_API SQLNativeSql(SQLHDBC hdbc, SQLCHAR* szSqlStrIn, SQLINTEGER cbSqlStrIn, SQLCHAR* szSqlStr,
                                              SQLINTEGER cbSqlStrMax, SQLINTEGER* pcbSqlStr) {
  return guardDbc(hdbc, "SQLNativeSql", [&](Connection& c) -> SQLRETURN {
    std::optional<std::string> in = textIn(szSqlStrIn, cbSqlStrIn, ansiEncodingOf(c));
    if (!in) throw DriverError(sqlstate::kInvalidNullPointer, "InStatementText is null");
    // Qualified: <wingdi.h> turns the bare name into a macro that collides with GDI's TextOutA.
    trino_odbc::TextOut out(szSqlStr, cbSqlStrMax, pcbSqlStr, ansiEncodingOf(c));
    return impl::nativeSql(c, std::move(*in), out);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLRowCount(SQLHSTMT hstmt, SQLLEN* pcrow) {
  return guardStmt(hstmt, "SQLRowCount", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::Allocated || s.state == StmtState::Prepared) {
      throw DriverError(sqlstate::kFunctionSequence, "SQLRowCount requires an executed statement");
    }
    if (pcrow) *pcrow = s.rowCount;
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLMoreResults(SQLHSTMT hstmt) {
  return guardStmt(hstmt, "SQLMoreResults", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::Allocated || s.state == StmtState::Prepared) {
      throw DriverError(sqlstate::kFunctionSequence, "SQLMoreResults requires an executed statement");
    }
    // Trino statements produce exactly one result: closing it is all there is to move past.
    s.closeCursor();
    return SQL_NO_DATA;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT hstmt) {
  return guardStmt(hstmt, "SQLCloseCursor", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    requireCursor(s);
    s.closeCursor();
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT hstmt, SQLUSMALLINT fOption) {
  // SQL_DROP destroys the handle; that cannot happen while guardStmt holds its mutex. The internal
  // helper is used rather than the exported SQLFreeHandle, which on Linux may bind to the driver
  // manager's function of the same name.
  if (fOption == SQL_DROP) return impl::freeStatementHandle(hstmt);
  return guardStmt(hstmt, "SQLFreeStmt", [&](Statement& s) -> SQLRETURN {
    switch (fOption) {
      case SQL_CLOSE:
        // Unlike SQLCloseCursor, closing without a cursor is not an error. A pending
        // data-at-execution sequence is abandoned as well (the DM allows SQL_CLOSE in S8).
        s.closeCursor();
        return SQL_SUCCESS;
      case SQL_UNBIND:
        requireNotBusy(s);
        s.unbindColumns();
        return SQL_SUCCESS;
      case SQL_RESET_PARAMS:
        requireNotBusy(s);
        s.resetParams();
        return SQL_SUCCESS;
      default:
        throw DriverError(sqlstate::kInvalidAttribute, "Invalid SQLFreeStmt option " + std::to_string(fOption));
    }
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT hstmt, SQLCHAR* szCursor, SQLSMALLINT cbCursorMax,
                                                  SQLSMALLINT* pcbCursor) {
  return guardStmt(hstmt, "SQLGetCursorName", [&](Statement& s) -> SQLRETURN {
    trino_odbc::TextOut out(szCursor, cbCursorMax, pcbCursor, ansiEncodingOf(s));
    return impl::getCursorName(s, out);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT hstmt, SQLCHAR* szCursor, SQLSMALLINT cbCursor) {
  return guardStmt(hstmt, "SQLSetCursorName", [&](Statement& s) -> SQLRETURN {
    std::optional<std::string> name = textIn(szCursor, cbCursor, ansiEncodingOf(s));
    if (!name) throw DriverError(sqlstate::kInvalidNullPointer, "CursorName is null");
    return impl::setCursorName(s, std::move(*name));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetPos(SQLHSTMT hstmt, SQLSETPOSIROW irow, SQLUSMALLINT fOption, SQLUSMALLINT fLock) {
  return guardStmt(hstmt, "SQLSetPos", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::Allocated || s.state == StmtState::Prepared) {
      // S1-S3 (no execution yet) is a function sequence error; 24000 is for executed statements.
      throw DriverError(sqlstate::kFunctionSequence, "Function sequence error: SQLSetPos requires an executed statement");
    }
    if (s.state != StmtState::Fetched || !s.result) {
      throw DriverError(sqlstate::kInvalidCursorState, "SQLSetPos requires a fetched rowset");
    }
    if (fOption != SQL_POSITION) {
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                        "SQLSetPos operation " + std::to_string(fOption) + " is not supported (read-only cursor)");
    }
    if (fLock != SQL_LOCK_NO_CHANGE) {
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented, "Row locking is not supported");
    }
    const size_t rows = s.result->rowsetSize();
    if (irow == 0) throw DriverError(sqlstate::kInvalidCursorPosition, "SQL_POSITION requires a row number (0 is the whole rowset)");
    if (static_cast<size_t>(irow) > rows) {
      throw DriverError(sqlstate::kRowValueOutOfRange,
                        formatString("Row %llu is outside the rowset of %zu row(s)", static_cast<unsigned long long>(irow), rows));
    }
    s.rowsetPos = static_cast<size_t>(irow);
    s.rowNumber = static_cast<SQLULEN>(s.result->rowsetStartRowNumber());  // SQL_ATTR_ROW_NUMBER adds rowsetPos-1
    s.getData.reset(s.result->columnCount());
    return SQL_SUCCESS;
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLBulkOperations(SQLHSTMT hstmt, SQLSMALLINT operation) {
  return guardStmt(hstmt, "SQLBulkOperations", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                      "SQLBulkOperations(" + std::to_string(operation) + ") is not supported (read-only cursor)");
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetScrollOptions(SQLHSTMT hstmt, SQLUSMALLINT fConcurrency, SQLLEN crowKeyset,
                                                     SQLUSMALLINT crowRowset) {
  return guardStmt(hstmt, "SQLSetScrollOptions", [&](Statement& s) -> SQLRETURN {
    requireNotBusy(s);
    if (s.state == StmtState::CursorOpen || s.state == StmtState::Fetched) {
      throw DriverError(sqlstate::kInvalidCursorState, "Cannot change scroll options while a cursor is open");
    }
    // Forward-only, read-only cursors: only the rowset size can be honoured; anything else is
    // reported as unsupported so ODBC 2 applications do not assume scrollability.
    if (fConcurrency != SQL_CONCUR_READ_ONLY || crowKeyset != SQL_SCROLL_FORWARD_ONLY) {
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented, "Only forward-only read-only cursors are supported");
    }
    // SQLSetScrollOptions sets the ODBC 2 rowset size (SQL_ROWSET_SIZE), used by SQLExtendedFetch
    // only; SQLFetch / SQLFetchScroll keep using SQL_ATTR_ROW_ARRAY_SIZE.
    if (crowRowset > 0) s.odbc2RowsetSize = crowRowset;
    return SQL_SUCCESS;
  });
}

}  // extern "C"
