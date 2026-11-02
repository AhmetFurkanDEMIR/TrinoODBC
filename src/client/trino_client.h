#pragma once
// Trino REST client: authentication, session state, statement protocol, spooled protocol,
// prefetching. See ARCHITECTURE.md §7. Module-private headers (http_client.h, oauth2.h,
// spool_decoder.h ...) may be added freely under src/client/.
#include "core/config.h"
#include "result/row_source.h"
#include "util/errors.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace trino_odbc {

/// Mutable per-connection state driven by X-Trino-Set-* response headers.
struct SessionState {
  std::string catalog, schema, path;
  std::map<std::string, std::string> sessionProperties;   // already URL-decoded values
  std::map<std::string, std::string> roles;               // catalog -> role spec
  std::map<std::string, std::string> preparedStatements;  // name -> sql (server-side registry mirror)
  std::string transactionId;                              // empty = none
  std::string authorizationUser;                          // X-Trino-Set-Authorization-User
  std::string timeZone, locale, source, clientInfo, traceToken, sessionUser;
  std::vector<std::string> clientTags;
  std::map<std::string, std::string> extraCredentials;
  mutable std::mutex mutex;
};

/// Consumer activity of one ODBC statement, shared by the statement and the queries it runs so that
/// IdleFetchTimeout can tell an application that is still reading from one that abandoned the
/// cursor. The statement's call tracker (core/handles.h) updates it on every API call entry and
/// exit; QueryExecution::nextPage() counts as activity too.
struct ConsumerActivity {
  /// steady_clock milliseconds of the last recorded activity.
  std::atomic<int64_t> lastMs{nowMs()};
  /// Number of statement API calls in progress (> 0: the application is inside the driver).
  std::atomic<int> inCall{0};
  /// Set when the statement's query was cancelled while no call was in progress (SQLCancel between
  /// two fetches, IdleFetchTimeout); the next call entry applies it to the open cursor.
  std::atomic<bool> cancelPending{false};
  void touch() { lastMs.store(nowMs(), std::memory_order_relaxed); }
  static int64_t nowMs();
};

/// Per-execution options.
struct ExecOptions {
  int queryTimeoutSec = 0;                 // 0 = connection default
  int64_t maxRows = 0;                     // stop fetching after N rows (0 = all)
  std::map<std::string, std::string> preparedStatements;  // extra X-Trino-Prepared-Statement entries for this request
  std::optional<std::string> transactionId;               // explicit override; nullopt = session value / NONE
  bool internal = false;                   // metadata query: no spooling, small pages, no prefetch thread
  bool startTransaction = false;           // send X-Trino-Transaction-Id: NONE and capture started id
  /// Optional external cancel request (e.g. Statement::cancelRequested) polled while execute() runs
  /// synchronously on the calling thread, before a QueryExecution exists to call cancel() on.
  const std::atomic<bool>* cancelFlag = nullptr;
  /// Activity of the consuming statement (IdleFetchTimeout); null = only nextPage() counts.
  std::shared_ptr<ConsumerActivity> activity;
};

struct QueryStats {
  std::string state;
  bool queued = false, scheduled = false;
  int64_t nodes = 0, totalSplits = 0, queuedSplits = 0, runningSplits = 0, completedSplits = 0;
  int64_t cpuTimeMillis = 0, wallTimeMillis = 0, queuedTimeMillis = 0, elapsedTimeMillis = 0;
  int64_t processedRows = 0, processedBytes = 0, physicalInputBytes = 0, peakMemoryBytes = 0, spilledBytes = 0;
  double progressPercentage = 0;
};

/// One running (or finished) query. Implements RowSource: nextPage() hands out decoded pages in
/// order; a prefetch thread keeps the bounded queue full. Cancel is safe from any thread.
class QueryExecution : public RowSource {
 public:
  ~QueryExecution() override;
  const std::vector<ResultColumn>& columns() const override { return columns_; }
  std::unique_ptr<Page> nextPage() override;
  void cancel() override;
  void close() override;
  std::optional<int64_t> updateCount() const override { return updateCount_; }
  std::string updateType() const override { return updateType_; }
  const std::vector<QueryWarning>& warnings() const override { return warnings_; }
  std::string queryId() const override { return queryId_; }
  const std::string& infoUri() const { return infoUri_; }
  QueryStats stats() const;
  /// True once the server reported FINISHED/FAILED and every page was handed out.
  bool finished() const { return finished_.load(); }
  /// True when the statement produced columns (SELECT / SHOW / DESCRIBE ...), false for DML/DDL.
  bool hasResultSet() const { return hasResultSet_; }
  /// True while the server-side query may still be running: the server has not reported a final
  /// response yet and neither cancel() nor close() (nor SQL_ATTR_MAX_ROWS) has stopped it.
  bool serverRunning() const;
  /// Cancels the query only when serverRunning(): sends DELETE nextUri and makes the next
  /// nextPage() throw CancelledError (HY008). Returns false, changing nothing, when the server
  /// query is already over (every page downloaded, failed, cancelled or closed).
  bool cancelIfRunning();
  /// True once cancel() / cancelIfRunning() / IdleFetchTimeout cancelled the query (nextPage()
  /// then throws CancelledError).
  bool wasCancelled() const;
  /// Message of the CancelledError nextPage() throws after a cancel.
  std::string cancelMessage() const;

 protected:
  friend class TrinoClient;
  QueryExecution() = default;
  std::vector<ResultColumn> columns_;
  std::optional<int64_t> updateCount_;
  std::string updateType_;
  std::vector<QueryWarning> warnings_;
  std::string queryId_, infoUri_;
  std::atomic<bool> finished_{false};
  bool hasResultSet_ = false;
  /// Transport, prefetch thread and page queue (module-private; defined in trino_client.cpp).
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Owns the transport for one ODBC connection.
class TrinoClient {
 public:
  TrinoClient(const ConnectionConfig& config, std::shared_ptr<SessionState> session);
  ~TrinoClient();
  TrinoClient(const TrinoClient&) = delete;
  TrinoClient& operator=(const TrinoClient&) = delete;

  /// Prepares auth (OAuth2 browser flow, token file, Kerberos setup), then validates the connection
  /// with `SELECT version()` unless disabled. Throws HttpError / DriverError (28000, 08001, HYT01...).
  void connect();
  void disconnect();
  bool connected() const { return connected_; }
  const std::string& serverVersion() const { return serverVersion_; }

  /// Starts a statement; returns once columns are known (or the query finished / failed).
  std::unique_ptr<QueryExecution> execute(std::string sql, const ExecOptions& opts = {});
  /// Runs a small internal statement fully into memory (metadata / DESCRIBE / USE / version).
  std::unique_ptr<MemoryRowSource> executeToMemory(std::string sql, ExecOptions opts = {});
  /// Cancels every query started through this client that is still running.
  void cancelAll();

  SessionState& session() { return *session_; }
  const ConnectionConfig& config() const { return config_; }
  const TypeMappingOptions& typeMapping() const { return typeMapping_; }
  /// Encodings actually compiled in and enabled (subset of config.queryDataEncoding).
  const std::string& queryDataEncoding() const { return queryDataEncoding_; }

  /// Process-wide libcurl initialisation (idempotent).
  static void globalInit();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ConnectionConfig config_;
  std::shared_ptr<SessionState> session_;
  TypeMappingOptions typeMapping_;
  std::string serverVersion_;
  std::string queryDataEncoding_;
  bool connected_ = false;
};

}  // namespace trino_odbc
