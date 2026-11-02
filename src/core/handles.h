#pragma once
// ODBC handle objects (environment, connection, statement, descriptor), the handle registry and
// the statement state machine. See ARCHITECTURE.md §4 and §8.
#include "platform.h"
#include "client/trino_client.h"
#include "convert/convert.h"
#include "core/config.h"
#include "core/diag.h"
#include "result/result_set.h"
#include "result/row_source.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace trino_odbc {

constexpr uint32_t kEnvMagic = 0x454E5648;   // 'ENVH'
constexpr uint32_t kDbcMagic = 0x44424348;   // 'DBCH'
constexpr uint32_t kStmtMagic = 0x53544D48;  // 'STMH'
constexpr uint32_t kDescMagic = 0x44455348;  // 'DESH'
constexpr uint32_t kFreedMagic = 0xDEADBEEF;

struct Environment;
struct Connection;
struct Statement;
struct Descriptor;

// ---- Handle registry ----------------------------------------------------------------------------
/// Global set of live handles; used to reject freed/garbage handles with SQL_INVALID_HANDLE.
class HandleRegistry {
 public:
  static HandleRegistry& global();
  void add(void* h);
  void remove(void* h);
  bool contains(void* h) const;

 private:
  mutable std::mutex mutex_;
  std::set<void*> handles_;
};

/// Locks a handle's recursive mutex for the duration of an API call.
class HandleLock {
 public:
  explicit HandleLock(std::recursive_mutex& m) : lock_(m) {}

 private:
  std::lock_guard<std::recursive_mutex> lock_;
};

// ---- Descriptor --------------------------------------------------------------------------------
enum class DescKind { Ard, Apd, Ird, Ipd };

/// One descriptor record (1-based in the API, 0-based in `records`).
struct DescRecord {
  // Common fields
  SQLSMALLINT type = SQL_C_DEFAULT;          // SQL_DESC_TYPE (verbose)
  SQLSMALLINT conciseType = SQL_C_DEFAULT;   // SQL_DESC_CONCISE_TYPE
  SQLSMALLINT datetimeIntervalCode = 0;      // SQL_DESC_DATETIME_INTERVAL_CODE
  SQLINTEGER datetimeIntervalPrecision = 0;
  SQLLEN octetLength = 0;                    // SQL_DESC_OCTET_LENGTH (buffer length for ARD/APD)
  SQLLEN length = 0;                         // SQL_DESC_LENGTH
  SQLSMALLINT precision = 0;                 // SQL_DESC_PRECISION
  SQLSMALLINT scale = 0;                     // SQL_DESC_SCALE
  SQLINTEGER numPrecRadix = 0;
  SQLSMALLINT nullable = SQL_NULLABLE;
  SQLSMALLINT unnamed = SQL_UNNAMED;
  std::string name;
  // Application pointers (ARD/APD)
  SQLPOINTER dataPtr = nullptr;
  SQLLEN* octetLengthPtr = nullptr;
  SQLLEN* indicatorPtr = nullptr;
  // IPD
  SQLSMALLINT parameterType = SQL_PARAM_INPUT;
  // IRD read-only metadata
  SQLLEN displaySize = 0;
  SQLSMALLINT searchable = SQL_PRED_SEARCHABLE;
  SQLSMALLINT unsignedAttr = SQL_FALSE;
  SQLSMALLINT caseSensitive = SQL_FALSE;
  SQLSMALLINT fixedPrecScale = SQL_FALSE;
  SQLSMALLINT autoUniqueValue = SQL_FALSE;
  SQLSMALLINT updatable = SQL_ATTR_READONLY;
  std::string typeName, localTypeName, literalPrefix, literalSuffix;
  std::string baseColumnName, baseTableName, tableName, schemaName, catalogName, label;
  // Driver-private bookkeeping
  bool bound = false;                        // dataPtr or indicatorPtr set through SQLBindCol/SQLBindParameter
  SQLSMALLINT boundSqlType = SQL_UNKNOWN_TYPE; // SQLBindParameter ParameterType (APD mirror for convenience)
  SQLULEN boundColumnSize = 0;
  SQLSMALLINT boundDecimalDigits = 0;
  bool precisionSet = false, scaleSet = false; // explicit SQLSetDescField on ARD (SQL_C_NUMERIC)
  bool autoPopulated = false;                // IPD record filled from DESCRIBE INPUT (dropped on re-prepare)
};

struct DescHeader {
  SQLSMALLINT allocType = SQL_DESC_ALLOC_AUTO;
  SQLULEN arraySize = 1;                     // SQL_DESC_ARRAY_SIZE (rowset / paramset size)
  SQLUSMALLINT* arrayStatusPtr = nullptr;    // SQL_DESC_ARRAY_STATUS_PTR
  SQLLEN* bindOffsetPtr = nullptr;           // SQL_DESC_BIND_OFFSET_PTR
  SQLINTEGER bindType = SQL_BIND_BY_COLUMN;  // SQL_DESC_BIND_TYPE
  SQLULEN* rowsProcessedPtr = nullptr;       // SQL_DESC_ROWS_PROCESSED_PTR
};

struct Descriptor {
  uint32_t magic = kDescMagic;
  DescKind kind = DescKind::Ard;
  DescHeader header;
  std::vector<DescRecord> records;           // records[i] is record i+1
  Connection* conn = nullptr;
  Statement* implicitOwner = nullptr;        // null for explicitly allocated descriptors
  std::set<Statement*> usedBy;               // statements whose ARD/APD points here (explicit only)
  Diag diag;

  bool isExplicit() const { return implicitOwner == nullptr; }
  bool isApplication() const { return kind == DescKind::Ard || kind == DescKind::Apd; }
  SQLSMALLINT count() const { return static_cast<SQLSMALLINT>(records.size()); }
  /// Grows so that record `index1` exists and returns it.
  DescRecord& record(SQLSMALLINT index1);
  const DescRecord* recordIf(SQLSMALLINT index1) const;
  /// Sets SQL_DESC_COUNT (drops records above n).
  void setCount(SQLSMALLINT n);
  void clearRecords() { records.clear(); }
  /// Rebuilds an IRD from result columns.
  void populateFromColumns(const std::vector<ResultColumn>& columns);
};

// ---- Statement ---------------------------------------------------------------------------------
enum class StmtState {
  Allocated,          // S1
  Prepared,           // S2/S3 prepared, no result
  ExecutedNoCursor,   // S4: executed, statement did not create a result set
  CursorOpen,         // S5: result set exists, cursor before first row
  Fetched,            // S6/S7: cursor positioned on a rowset
  NeedData,           // S8-S10: SQLExecute/SQLExecDirect returned SQL_NEED_DATA
  Executing           // S11: a call is in flight (used for SQLCancel semantics)
};

/// Parameter metadata from DESCRIBE INPUT.
struct ParamInfo {
  int position = 0;
  TrinoType type;      // Unknown when Trino reported "unknown"
};

/// Data-at-execution bookkeeping.
struct DaeState {
  bool active = false;
  SQLSMALLINT currentParam = 0;      // 1-based parameter awaiting SQLPutData (0 = none)
  SQLULEN paramSet = 0;              // current parameter set (0-based)
  std::vector<std::string> buffers;  // accumulated bytes per parameter (index = param-1)
  std::vector<bool> provided;        // whether SQLPutData delivered data for param
  std::vector<bool> nulls;           // SQLPutData delivered SQL_NULL_DATA for param (index = param-1)
  bool putDataCalled = false;
};

/// "Function in progress" counter of a statement with the atomic<int> interface guardStmt uses
/// (load / fetch_add / fetch_sub, always under the statement mutex). Besides counting it
///  * records every entry and exit of a statement call in the shared ConsumerActivity, so a query
///    can tell whether the application is still using the statement (IdleFetchTimeout) without any
///    hook in the individual fetch functions, and
///  * on the outermost entry applies a cancel that happened while no call was running (SQLCancel
///    between two fetches, IdleFetchTimeout) to the open cursor (Statement::applyPendingCancel), so
///    that very call already sees the cursor as cancelled (HY008) instead of rows still buffered.
class StatementCallTracker {
 public:
  explicit StatementCallTracker(Statement& owner) : owner_(owner) {}
  StatementCallTracker(const StatementCallTracker&) = delete;
  StatementCallTracker& operator=(const StatementCallTracker&) = delete;
  int load() const { return activity_->inCall.load(); }
  int fetch_add(int n);  // core/handles.cpp
  int fetch_sub(int n) {
    activity_->touch();
    return activity_->inCall.fetch_sub(n);
  }
  const std::shared_ptr<ConsumerActivity>& activity() const { return activity_; }

 private:
  Statement& owner_;
  std::shared_ptr<ConsumerActivity> activity_ = std::make_shared<ConsumerActivity>();
};

struct Statement {
  uint32_t magic = kStmtMagic;
  Connection* conn = nullptr;
  std::recursive_mutex mutex;
  Diag diag;
  StmtState state = StmtState::Allocated;

  // Descriptors: implicit ones are owned; ard/apd may point to explicit descriptors.
  Descriptor implicitArd, implicitApd, implicitIrd, implicitIpd;
  Descriptor* ard = &implicitArd;
  Descriptor* apd = &implicitApd;
  Descriptor* ird = &implicitIrd;
  Descriptor* ipd = &implicitIpd;

  // Statement attributes (those not stored in descriptors)
  SQLULEN queryTimeout = 0;
  SQLULEN maxRows = 0;
  SQLULEN maxLength = 0;
  SQLULEN cursorType = SQL_CURSOR_FORWARD_ONLY;
  SQLULEN concurrency = SQL_CONCUR_READ_ONLY;
  SQLULEN cursorScrollable = SQL_NONSCROLLABLE;
  SQLULEN cursorSensitivity = SQL_UNSPECIFIED;
  SQLULEN noscan = SQL_NOSCAN_OFF;
  SQLULEN retrieveData = SQL_RD_ON;
  SQLULEN useBookmarks = SQL_UB_OFF;
  SQLULEN metadataId = SQL_FALSE;
  SQLULEN asyncEnable = SQL_ASYNC_ENABLE_OFF;
  SQLULEN keysetSize = 0;
  SQLULEN simulateCursor = SQL_SC_NON_UNIQUE;
  SQLULEN rowNumber = 0;             // SQL_ATTR_ROW_NUMBER (1-based current row, 0 = unknown)
  SQLULEN odbc2RowsetSize = 1;       // SQL_ROWSET_SIZE / SQLSetScrollOptions: rowset of SQLExtendedFetch only

  std::string cursorName;
  bool cursorNameExplicit = false;

  // SQL text
  std::string originalSql;           // as given by the application (UTF-8)
  std::string sql;                   // after escape translation
  size_t paramCount = 0;
  std::string prepName;              // server-side prepared statement name for DESCRIBE (odbc_stmt_<n>)
  bool prepared = false;             // SQLPrepare done (vs SQLExecDirect)
  std::vector<std::string> translateWarnings;

  // Cached metadata (valid until re-prepare)
  std::vector<ResultColumn> describedColumns;
  bool describedOutputValid = false;
  bool describedOutputIsResultSet = true;
  std::vector<ParamInfo> describedParams;
  bool describedInputValid = false;

  // Execution results
  std::unique_ptr<ResultSet> result;
  SQLLEN rowCount = -1;              // SQLRowCount (-1 = unknown / n/a)
  std::string queryId;
  GetDataState getData;
  size_t rowsetPos = 1;              // 1-based row inside the rowset used by SQLGetData (SQLSetPos)
  DaeState dae;
  SQLULEN currentParamSet = 0;       // for SQL_ATTR_PARAMSET_SIZE > 1 sequential execution
  std::atomic<bool> cancelRequested{false};
  /// Nesting depth of guarded API calls running on this statement (> 0 while a function is in
  /// progress); set/cleared by guardStmt under `mutex`. It also records the application's activity
  /// for IdleFetchTimeout (shared with the statement's queries through ExecOptions::activity).
  StatementCallTracker callDepth{*this};
  /// SQLGetData bytes already returned per column under SQL_ATTR_MAX_LENGTH; only meaningful while
  /// the matching getData.offsets entry is non-zero (a zero offset means a fresh column read).
  std::vector<SQLLEN> maxLengthDelivered;
  std::shared_ptr<QueryExecution> activeQuery;   // in-flight or open query (for SQLCancel)
  std::mutex activeQueryMutex;

  // ---- helpers (core/handles.cpp) --------------------------------------------------------------
  /// Columns of the current result if any, else the described columns.
  const std::vector<ResultColumn>& columns() const;
  bool hasResult() const { return result != nullptr; }
  /// Closes the cursor / discards results (SQL_CLOSE). Cancels a running query.
  void closeCursor();
  /// When the open cursor's query was cancelled while no call was running (and the cursor has not
  /// reached its end), replaces the result by one that reports that cancel (HY008) at every fetch,
  /// dropping the rows still buffered and stopping the query. Caller holds `mutex`. Never throws.
  void applyPendingCancel() noexcept;
  /// Discards the prepared statement text and cached metadata.
  void resetPrepared();
  /// SQL_UNBIND
  void unbindColumns();
  /// SQL_RESET_PARAMS
  void resetParams();
  /// Rebuilds ird from `columns`.
  void rebuildIrd(const std::vector<ResultColumn>& columns);
  /// Registers/unregisters the active query (thread-safe).
  void setActiveQuery(std::shared_ptr<QueryExecution> q);
  std::shared_ptr<QueryExecution> takeActiveQuery();
  /// Effective ANSI encoding / conversion options for this statement.
  ConvertOptions convertOptions() const;
};

// ---- Connection --------------------------------------------------------------------------------
struct Connection {
  uint32_t magic = kDbcMagic;
  Environment* env = nullptr;
  std::recursive_mutex mutex;
  Diag diag;
  ConnectionConfig config;
  std::shared_ptr<SessionState> session = std::make_shared<SessionState>();
  std::unique_ptr<TrinoClient> client;
  bool connected = false;
  std::string serverVersion;
  std::string dataSourceName;        // DSN used (for SQL_DATA_SOURCE_NAME)

  // Connection attributes
  SQLUINTEGER autocommit = SQL_AUTOCOMMIT_ON;
  SQLUINTEGER txnIsolation = SQL_TXN_READ_UNCOMMITTED;
  SQLUINTEGER accessMode = SQL_MODE_READ_WRITE;
  SQLUINTEGER loginTimeout = 0;
  SQLUINTEGER connectionTimeout = 0;
  SQLUINTEGER packetSize = 0;
  SQLUINTEGER metadataId = SQL_FALSE;
  SQLUINTEGER asyncEnable = SQL_ASYNC_ENABLE_OFF;
  SQLUINTEGER trace = SQL_OPT_TRACE_OFF;
  SQLUINTEGER odbcCursors = SQL_CUR_USE_DRIVER;
  SQLUINTEGER autoIpd = SQL_TRUE;
  SQLPOINTER quietMode = nullptr;
  std::string traceFile, translateLib;
  std::string pendingCatalog;        // SQL_ATTR_CURRENT_CATALOG set before connect
  bool inTransaction = false;        // driver-started transaction is open

  std::set<Statement*> statements;
  std::set<Descriptor*> descriptors; // explicitly allocated
  unsigned stmtCounter = 0;

  // ---- helpers (core/handles.cpp) --------------------------------------------------------------
  Statement* createStatement();
  void destroyStatement(Statement* s);
  Descriptor* createDescriptor();
  void destroyDescriptor(Descriptor* d);
  /// Closes every statement's cursor and cancels running queries.
  void closeAllStatements();
  ConvertOptions convertOptions() const;
  std::string currentCatalog() const;
  std::string currentSchema() const;
};

// ---- Environment -------------------------------------------------------------------------------
struct Environment {
  uint32_t magic = kEnvMagic;
  std::recursive_mutex mutex;
  Diag diag;
  SQLINTEGER odbcVersion = 0;        // must be set via SQL_ATTR_ODBC_VERSION before allocating a connection
  SQLINTEGER connectionPooling = SQL_CP_OFF;
  SQLINTEGER cpMatch = SQL_CP_STRICT_MATCH;
  SQLINTEGER outputNts = SQL_TRUE;
  std::set<Connection*> connections;

  Connection* createConnection();
  void destroyConnection(Connection* c);
};

// ---- Validation ----------------------------------------------------------------------------------
Environment* asEnvironment(SQLHANDLE h);
Connection* asConnection(SQLHANDLE h);
Statement* asStatement(SQLHANDLE h);
Descriptor* asDescriptor(SQLHANDLE h);

/// Allocation / destruction with registry bookkeeping.
Environment* allocEnvironment();
void freeEnvironment(Environment* env);

}  // namespace trino_odbc
