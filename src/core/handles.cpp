// ODBC handle objects: registry, allocation/destruction, descriptor records and the statement
// helper methods used by the API layer. See ARCHITECTURE.md §4 and §8.
#include "core/handles.h"

#include "util/errors.h"
#include "util/log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace trino_odbc {

namespace {

constexpr const char* kLogComponent = "handles";

/// Registers the four implicit descriptors of a statement so SQLGetDescField & co. accept the
/// handles returned by SQLGetStmtAttr(SQL_ATTR_IMP_ROW_DESC ...).
void initImplicitDescriptor(Descriptor& d, DescKind kind, Statement* owner) {
  d.magic = kDescMagic;
  d.kind = kind;
  d.conn = owner->conn;
  d.implicitOwner = owner;
  d.header.allocType = SQL_DESC_ALLOC_AUTO;
  if (owner->conn) {
    d.diag.serverName = owner->conn->config.host;
    d.diag.connectionName = owner->conn->dataSourceName.empty() ? owner->conn->config.dsn : owner->conn->dataSourceName;
  }
  HandleRegistry::global().add(&d);
}

void retireImplicitDescriptor(Descriptor& d) {
  HandleRegistry::global().remove(&d);
  d.magic = kFreedMagic;
}

/// Aborts whatever the statement is doing without taking its mutex: flags an in-progress call
/// (polled while execute() runs) and cancels the active query (wakes a blocked fetch).
void cancelInFlight(Statement& s) {
  if (s.callDepth.load() > 0) s.cancelRequested.store(true);
  std::shared_ptr<QueryExecution> q;
  {
    std::lock_guard<std::mutex> g(s.activeQueryMutex);
    q = s.activeQuery;
  }
  if (!q || q->finished()) return;
  try {
    q->cancel();
  } catch (const std::exception& e) {
    TRINO_LOG_WARN(kLogComponent, "cancelling query %s failed: %s", q->queryId().c_str(), e.what());
  }
}

/// Result of a cursor whose query was cancelled while the application was not inside the driver:
/// every fetch reports the cancel (HY008), the column metadata stays available.
class CancelledRowSource : public RowSource {
 public:
  CancelledRowSource(std::vector<ResultColumn> columns, std::string message, std::string queryId)
      : columns_(std::move(columns)), message_(std::move(message)), queryId_(std::move(queryId)) {}
  const std::vector<ResultColumn>& columns() const override { return columns_; }
  std::unique_ptr<Page> nextPage() override { throw CancelledError(message_); }
  std::string queryId() const override { return queryId_; }

 private:
  std::vector<ResultColumn> columns_;
  std::string message_;
  std::string queryId_;
};

template <typename T>
T* validateHandle(SQLHANDLE h, uint32_t magic) {
  if (h == nullptr) return nullptr;
  // Registry membership is checked before touching memory: a garbage pointer must never be
  // dereferenced, and a freed handle is removed from the registry before its memory goes away.
  if (!HandleRegistry::global().contains(h)) return nullptr;
  T* obj = static_cast<T*>(h);
  return obj->magic == magic ? obj : nullptr;
}

}  // namespace

// ---- HandleRegistry ----------------------------------------------------------------------------

HandleRegistry& HandleRegistry::global() {
  // Leaked on purpose: handles may be freed from DllMain / atexit after static destruction began.
  static HandleRegistry* instance = new HandleRegistry();
  return *instance;
}

void HandleRegistry::add(void* h) {
  if (!h) return;
  std::lock_guard<std::mutex> lock(mutex_);
  handles_.insert(h);
}

void HandleRegistry::remove(void* h) {
  if (!h) return;
  std::lock_guard<std::mutex> lock(mutex_);
  handles_.erase(h);
}

bool HandleRegistry::contains(void* h) const {
  if (!h) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return handles_.find(h) != handles_.end();
}

// ---- Descriptor --------------------------------------------------------------------------------

DescRecord& Descriptor::record(SQLSMALLINT index1) {
  // Callers validate the index (07009). A non-positive index (e.g. an SQLUSMALLINT column number
  // above 32767 wrapped by a cast) must never alias record 1, so it is rejected here as well.
  if (index1 < 1) {
    throw DriverError(sqlstate::kInvalidDescriptorIndex,
                      "Invalid descriptor index " + std::to_string(static_cast<int>(index1)));
  }
  const size_t idx = static_cast<size_t>(index1 - 1);
  if (idx >= records.size()) records.resize(idx + 1);
  return records[idx];
}

const DescRecord* Descriptor::recordIf(SQLSMALLINT index1) const {
  if (index1 < 1 || static_cast<size_t>(index1) > records.size()) return nullptr;
  return &records[static_cast<size_t>(index1 - 1)];
}

void Descriptor::setCount(SQLSMALLINT n) {
  const size_t count = n < 0 ? 0 : static_cast<size_t>(n);
  records.resize(count);
}

void Descriptor::populateFromColumns(const std::vector<ResultColumn>& columns) {
  records.clear();
  records.reserve(columns.size());
  for (const ResultColumn& col : columns) {
    const OdbcTypeInfo& t = col.odbc;
    DescRecord r;
    r.type = t.verboseType;
    r.conciseType = t.conciseType;
    r.datetimeIntervalCode = t.intervalType ? t.intervalCode : t.datetimeSubcode;
    // SQL_DESC_DATETIME_INTERVAL_PRECISION is the leading-field precision of intervals only
    // (2 for year-month, 9 for day-second, matching SQLGetTypeInfo INTERVAL_PRECISION).
    r.datetimeIntervalPrecision = 0;
    if (t.intervalType) {
      r.datetimeIntervalPrecision = t.category == TypeCategory::IntervalYearToMonth ? 2 : 9;
    }
    r.octetLength = t.octetLength;
    r.length = static_cast<SQLLEN>(t.columnSize);
    // For datetime types SQL_DESC_PRECISION is the fractional-seconds precision; the type mapper
    // reports it in decimalDigits, so fall back to that when precision was left at 0.
    r.precision = t.precision;
    if (r.precision == 0 && t.decimalDigits > 0 && t.verboseType == SQL_DATETIME) r.precision = t.decimalDigits;
    r.scale = t.decimalDigits;
    r.numPrecRadix = t.numPrecRadix;
    r.nullable = t.nullable;
    r.unnamed = col.name.empty() ? SQL_UNNAMED : SQL_NAMED;
    r.name = col.name;
    r.label = col.name;
    r.dataPtr = nullptr;
    r.octetLengthPtr = nullptr;
    r.indicatorPtr = nullptr;
    r.parameterType = SQL_PARAM_INPUT;
    r.displaySize = t.displaySize;
    r.searchable = t.searchable;
    r.unsignedAttr = t.unsignedAttr ? SQL_TRUE : SQL_FALSE;
    r.caseSensitive = t.caseSensitive ? SQL_TRUE : SQL_FALSE;
    r.fixedPrecScale = t.fixedPrecScale ? SQL_TRUE : SQL_FALSE;
    r.autoUniqueValue = t.autoIncrement ? SQL_TRUE : SQL_FALSE;
    r.updatable = SQL_ATTR_READONLY;
    r.typeName = t.typeName;
    r.localTypeName = t.localTypeName.empty() ? t.typeName : t.localTypeName;
    r.literalPrefix = t.literalPrefix;
    r.literalSuffix = t.literalSuffix;
    // Base names describe the underlying table column; an aliased expression has no base column.
    r.baseColumnName = col.aliased ? std::string() : col.name;
    r.baseTableName = col.table;
    r.tableName = col.table;
    r.schemaName = col.schema;
    r.catalogName = col.catalog;
    r.bound = false;
    r.boundSqlType = t.conciseType;
    r.boundColumnSize = t.columnSize;
    r.boundDecimalDigits = t.decimalDigits;
    r.precisionSet = false;
    r.scaleSet = false;
    records.push_back(std::move(r));
  }
}

// ---- Statement ---------------------------------------------------------------------------------

const std::vector<ResultColumn>& Statement::columns() const {
  if (result) return result->columns();
  return describedColumns;
}

void Statement::closeCursor() {
  // Cancel first so a prefetch thread blocked on the network wakes up before the result is torn down.
  if (auto q = takeActiveQuery()) {
    if (!q->finished()) {
      TRINO_LOG_DEBUG(kLogComponent, "closing cursor cancels running query %s", q->queryId().c_str());
      q->cancel();
    }
  }
  if (result) {
    result->close();
    result.reset();
  }
  getData.offsets.clear();
  rowsetPos = 1;
  rowNumber = 0;
  // cancelRequested is deliberately left alone: a SQLCancel that arrived while the current call
  // (e.g. SQLExecDirect, which closes the previous cursor first) was running must survive until
  // that call polls it. guardStmt clears the flag when the next call starts.
  dae = DaeState{};
  currentParamSet = 0;

  if (prepared) {
    // Back to S2/S3: the prepared statement (and its described IRD, if known) stays available.
    state = StmtState::Prepared;
    if (describedOutputValid) ird->populateFromColumns(describedColumns);
  } else {
    state = StmtState::Allocated;
    ird->clearRecords();
  }
}

void Statement::applyPendingCancel() noexcept {
  try {
    if (!result || result->atEnd()) return;  // nothing left to read: SQL_NO_DATA stays correct
    std::shared_ptr<QueryExecution> q;
    {
      std::lock_guard<std::mutex> lock(activeQueryMutex);
      q = activeQuery;
    }
    if (!q || !q->wasCancelled()) return;
    std::string message = q->cancelMessage();
    std::vector<ResultColumn> cols = result->columns();
    const std::string id = q->queryId();
    // Stops the (already cancelled) query, joins its prefetch thread and frees the buffered pages.
    result->close();
    setActiveQuery(nullptr);
    result = std::make_unique<ResultSet>(std::make_unique<CancelledRowSource>(std::move(cols), std::move(message), id));
    getData.offsets.clear();
    rowsetPos = 1;
    TRINO_LOG_DEBUG(kLogComponent, "cursor of query %s marked as cancelled", id.c_str());
  } catch (const std::exception& e) {
    TRINO_LOG_WARN(kLogComponent, "applying a cancel to the open cursor failed: %s", e.what());
  } catch (...) {
  }
}

int StatementCallTracker::fetch_add(int n) {
  activity_->touch();
  const int before = activity_->inCall.fetch_add(n);
  if (before == 0 && activity_->cancelPending.exchange(false)) owner_.applyPendingCancel();
  return before;
}

void Statement::resetPrepared() {
  originalSql.clear();
  sql.clear();
  paramCount = 0;
  prepName.clear();
  prepared = false;
  translateWarnings.clear();
  describedColumns.clear();
  describedOutputValid = false;
  describedOutputIsResultSet = true;
  describedParams.clear();
  describedInputValid = false;
  ird->clearRecords();
  // IPD records written by SQLBindParameter / SQLSetDescField survive a re-prepare (bindings
  // persist across prepares); only the ones auto-populated from DESCRIBE INPUT describe the old
  // statement and are dropped.
  for (DescRecord& r : ipd->records) {
    if (r.autoPopulated) r = DescRecord{};
  }
  size_t keep = ipd->records.size();
  while (keep > 0) {
    const DescRecord& r = ipd->records[keep - 1];
    if (r.bound || r.conciseType != SQL_C_DEFAULT || r.autoPopulated) break;
    --keep;
  }
  ipd->records.resize(keep);
  if (!result) state = StmtState::Allocated;
}

void Statement::unbindColumns() {
  // SQL_UNBIND sets SQL_DESC_COUNT of the ARD to 0, which releases every column binding
  // (including a bookmark binding).
  ard->setCount(0);
}

void Statement::resetParams() {
  // SQL_RESET_PARAMS only sets SQL_DESC_COUNT of the APD to 0 (ODBC); the IPD (bound or
  // auto-populated parameter descriptions) stays as it is.
  apd->setCount(0);
  dae = DaeState{};
  currentParamSet = 0;
  if (state == StmtState::NeedData) state = prepared ? StmtState::Prepared : StmtState::Allocated;
}

void Statement::rebuildIrd(const std::vector<ResultColumn>& cols) {
  ird->populateFromColumns(cols);
}

void Statement::setActiveQuery(std::shared_ptr<QueryExecution> q) {
  std::lock_guard<std::mutex> lock(activeQueryMutex);
  activeQuery = std::move(q);
}

std::shared_ptr<QueryExecution> Statement::takeActiveQuery() {
  std::lock_guard<std::mutex> lock(activeQueryMutex);
  return std::move(activeQuery);
}

ConvertOptions Statement::convertOptions() const {
  if (conn) return conn->convertOptions();
  return ConvertOptions{};
}

// ---- Connection --------------------------------------------------------------------------------

Statement* Connection::createStatement() {
  Statement* s = new Statement();
  s->conn = this;
  initImplicitDescriptor(s->implicitArd, DescKind::Ard, s);
  initImplicitDescriptor(s->implicitApd, DescKind::Apd, s);
  initImplicitDescriptor(s->implicitIrd, DescKind::Ird, s);
  initImplicitDescriptor(s->implicitIpd, DescKind::Ipd, s);
  s->ard = &s->implicitArd;
  s->apd = &s->implicitApd;
  s->ird = &s->implicitIrd;
  s->ipd = &s->implicitIpd;
  s->ard->header.arraySize = 1;
  s->apd->header.arraySize = 1;
  s->diag.serverName = config.host;
  s->diag.connectionName = dataSourceName.empty() ? config.dsn : dataSourceName;
  // Default cursor names are SQL_CUR<n>, unique per connection, as the spec prescribes.
  s->cursorName = "SQL_CUR" + std::to_string(++stmtCounter);
  s->cursorNameExplicit = false;
  HandleRegistry::global().add(s);
  statements.insert(s);
  TRINO_LOG_TRACE(kLogComponent, "statement %p allocated on connection %p", static_cast<void*>(s),
                  static_cast<void*>(this));
  return s;
}

void Connection::destroyStatement(Statement* s) {
  if (!s) return;
  // Cancel before locking: a call blocked on the network (SQLFetch waiting for a page) holds the
  // statement mutex and only returns once its query is cancelled.
  cancelInFlight(*s);
  {
    HandleLock lock(s->mutex);
    // A destroy path must never throw: the caller is SQLFreeHandle, which has no way to report it.
    try {
      s->closeCursor();
    } catch (const std::exception& e) {
      TRINO_LOG_WARN(kLogComponent, "closing cursor while freeing statement failed: %s", e.what());
    }
    // Explicit descriptors keep a back-reference so freeing them can revert statements; drop it.
    for (Descriptor* d : descriptors) d->usedBy.erase(s);
    retireImplicitDescriptor(s->implicitArd);
    retireImplicitDescriptor(s->implicitApd);
    retireImplicitDescriptor(s->implicitIrd);
    retireImplicitDescriptor(s->implicitIpd);
    HandleRegistry::global().remove(s);
    s->magic = kFreedMagic;
  }
  statements.erase(s);
  TRINO_LOG_TRACE(kLogComponent, "statement %p freed", static_cast<void*>(s));
  delete s;
}

Descriptor* Connection::createDescriptor() {
  Descriptor* d = new Descriptor();
  d->magic = kDescMagic;
  d->kind = DescKind::Ard;  // an explicit descriptor acts as ARD or APD depending on where it is attached
  d->conn = this;
  d->implicitOwner = nullptr;
  d->header.allocType = SQL_DESC_ALLOC_USER;
  d->diag.serverName = config.host;
  d->diag.connectionName = dataSourceName.empty() ? config.dsn : dataSourceName;
  HandleRegistry::global().add(d);
  descriptors.insert(d);
  return d;
}

void Connection::destroyDescriptor(Descriptor* d) {
  if (!d) return;
  // Statements bound to this explicit descriptor fall back to their implicit ones (ODBC spec).
  for (Statement* s : d->usedBy) {
    if (s->ard == d) s->ard = &s->implicitArd;
    if (s->apd == d) s->apd = &s->implicitApd;
  }
  d->usedBy.clear();
  HandleRegistry::global().remove(d);
  d->magic = kFreedMagic;
  descriptors.erase(d);
  delete d;
}

void Connection::closeAllStatements() {
  // Cancel first, lock afterwards: a statement blocked in SQLFetch/SQLExecute holds its mutex until
  // its query is cancelled, so locking first would wait for that call to finish on its own.
  if (client) {
    try {
      client->cancelAll();
    } catch (const std::exception& e) {
      TRINO_LOG_WARN(kLogComponent, "cancelling running queries failed: %s", e.what());
    }
  }
  for (Statement* s : statements) cancelInFlight(*s);
  for (Statement* s : statements) {
    HandleLock lock(s->mutex);
    try {
      s->closeCursor();
    } catch (const std::exception& e) {
      TRINO_LOG_WARN(kLogComponent, "closing cursor of statement %p failed: %s", static_cast<void*>(s), e.what());
    }
  }
}

ConvertOptions Connection::convertOptions() const {
  ConvertOptions o;
  o.ansi = config.ansiEncoding;
  o.timestampTz = config.timestampTzMode;
  o.timeTz = config.timeTzMode;
  o.interval = config.intervalMode;
  o.sessionTimeZone = resolveSessionTimeZone(config.timeZone);
  if (session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->timeZone.empty()) o.sessionTimeZone = session->timeZone;
  }
  return o;
}

std::string Connection::currentCatalog() const {
  if (session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->catalog.empty()) return session->catalog;
  }
  if (!pendingCatalog.empty()) return pendingCatalog;
  return config.catalog;
}

std::string Connection::currentSchema() const {
  if (session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->schema.empty()) return session->schema;
  }
  return config.schema;
}

// ---- Environment -------------------------------------------------------------------------------

Connection* Environment::createConnection() {
  Connection* c = new Connection();
  c->env = this;
  HandleRegistry::global().add(c);
  connections.insert(c);
  TRINO_LOG_TRACE(kLogComponent, "connection %p allocated", static_cast<void*>(c));
  return c;
}

void Environment::destroyConnection(Connection* c) {
  if (!c) return;
  {
    HandleLock lock(c->mutex);
    c->closeAllStatements();
    // Copy: destroyStatement/destroyDescriptor erase from the sets while we iterate.
    const std::vector<Statement*> stmts(c->statements.begin(), c->statements.end());
    for (Statement* s : stmts) c->destroyStatement(s);
    const std::vector<Descriptor*> descs(c->descriptors.begin(), c->descriptors.end());
    for (Descriptor* d : descs) c->destroyDescriptor(d);
    if (c->client) {
      try {
        c->client->disconnect();
      } catch (const std::exception& e) {
        TRINO_LOG_WARN(kLogComponent, "disconnect while freeing connection failed: %s", e.what());
      }
      c->client.reset();
    }
    c->connected = false;
    HandleRegistry::global().remove(c);
    c->magic = kFreedMagic;
  }
  connections.erase(c);
  TRINO_LOG_TRACE(kLogComponent, "connection %p freed", static_cast<void*>(c));
  delete c;
}

// ---- Validation / allocation --------------------------------------------------------------------

Environment* asEnvironment(SQLHANDLE h) { return validateHandle<Environment>(h, kEnvMagic); }
Connection* asConnection(SQLHANDLE h) { return validateHandle<Connection>(h, kDbcMagic); }
Statement* asStatement(SQLHANDLE h) { return validateHandle<Statement>(h, kStmtMagic); }
Descriptor* asDescriptor(SQLHANDLE h) { return validateHandle<Descriptor>(h, kDescMagic); }

Environment* allocEnvironment() {
  Environment* env = new Environment();
  HandleRegistry::global().add(env);
  TRINO_LOG_TRACE(kLogComponent, "environment %p allocated", static_cast<void*>(env));
  return env;
}

void freeEnvironment(Environment* env) {
  if (!env) return;
  {
    HandleLock lock(env->mutex);
    // The API layer refuses to free an environment with live connections (HY010); anything still
    // here is cleaned up defensively so no handle leaks or dangles.
    const std::vector<Connection*> conns(env->connections.begin(), env->connections.end());
    for (Connection* c : conns) env->destroyConnection(c);
    HandleRegistry::global().remove(env);
    env->magic = kFreedMagic;
  }
  TRINO_LOG_TRACE(kLogComponent, "environment %p freed", static_cast<void*>(env));
  delete env;
}

}  // namespace trino_odbc
