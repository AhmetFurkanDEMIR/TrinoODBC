// Trino REST client: connection setup (auth, validation), the statement protocol loop, session
// state driven by response headers, the prefetch thread with its bounded page queue, spooled
// segment orchestration and cancel/close semantics. See trino_client.h, ARCHITECTURE.md §7 and
// docs/design/CLIENT-DETAILS.md.
//
// Threading model of one QueryExecution:
//  * execute() runs the POST and follows nextUri on the calling thread until the column list and
//    the first page are known ("sync phase"). Only that thread touches the HttpClient then.
//  * Afterwards the same HttpClient is handed to the prefetch thread (or, for internal queries
//    without a thread, to whoever calls nextPage()). Pages flow through `queue` under `mutex`.
//  * cancel() is lock-light: it flips the cancel flag (curl's progress callback aborts transfers),
//    wakes every waiter and sends DELETE nextUri from a detached helper. It never joins. The
//    DELETE goes out at most once per query and only while the server query may still run.
//  * close() cancels whatever is unfinished, joins the prefetch thread and the segment workers,
//    frees the pages and unregisters the query. The destructor calls close().
//  * IdleFetchTimeout: while the server query runs, the prefetch thread checks (between requests
//    and every poll interval while it waits for queue room) how long the consuming statement has
//    been left alone; past the limit it cancels the query and the next fetch reports HY008.
#include "client/trino_client.h"

#include "client/http_client.h"
#include "client/oauth2.h"
#include "client/spool_decoder.h"
#include "core/diag.h"
#include "result/json_decoder.h"
#include "util/log.h"
#include "util/strings.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>
#include <utility>

namespace trino_odbc {

namespace client_internal {

constexpr const char* kLog = "client";
constexpr const char* kStatementPath = "/v1/statement";
constexpr const char* kClientCapabilities = "PARAMETRIC_DATETIME,SESSION_AUTHORIZATION";
/// Total time budget of one cancel DELETE (connect included); a transport failure is retried once.
constexpr int kCancelTimeoutSec = 5;
constexpr int kCancelAttempts = 2;
constexpr int kCancelRetryDelayMs = 200;
/// How long disconnect() waits for this connection's outstanding cancel DELETEs, so that an
/// application which exits right after SQLDisconnect / SQLFreeHandle still reaches the server.
constexpr int kCancelDrainMs = 3000;
/// Poll interval bounds of the IdleFetchTimeout check while the producer waits for queue room.
constexpr int64_t kIdlePollMinMs = 50;
constexpr int64_t kIdlePollMaxMs = 1000;
constexpr int kMaxTargetResultSizeMb = 128;
constexpr size_t kLoggedSqlLimit = 2000;

/// Registry of the executions started through one client, so that cancelAll() (SQLDisconnect,
/// SQLCancelHandle on the connection) can reach queries whose statements are busy elsewhere.
/// Executions register before their first request and unregister in close(); the registry is
/// shared by the client and every execution so neither lifetime constrains the other.
class QueryRegistry {
 public:
  void add(QueryExecution* q) {
    std::lock_guard<std::mutex> g(mutex_);
    queries_.insert(q);
  }
  void remove(QueryExecution* q) {
    std::lock_guard<std::mutex> g(mutex_);
    queries_.erase(q);
  }
  /// Cancels every registered query. QueryExecution::cancel() never touches the registry, so
  /// holding the registry lock while calling it cannot deadlock; an execution being destroyed
  /// blocks in remove() until this loop is done, which keeps every pointer here valid.
  void cancelAll() {
    std::lock_guard<std::mutex> g(mutex_);
    for (QueryExecution* q : queries_) q->cancel();
  }

 private:
  std::mutex mutex_;
  std::set<QueryExecution*> queries_;
};

/// Everything a query needs from its connection, kept alive by shared_ptr so an execution (and
/// the detached cancel helpers) can outlive the TrinoClient that started it.
struct ClientShared {
  std::shared_ptr<const ConnectionConfig> config;
  std::shared_ptr<CurlShare> share;
  std::shared_ptr<AuthContext> auth;
  std::shared_ptr<SessionState> session;
  std::shared_ptr<QueryRegistry> registry;
  TypeMappingOptions typeMapping;
  std::string statementUrl;
  std::string queryDataEncoding;      // config list filtered by the compiled-in codecs
  std::string oauthCacheKey;
  /// False when oauthCacheKey is private to this connection (no UID): its tokens then live only in
  /// `auth`, so they are released with the connection instead of accumulating in the process cache.
  bool oauthUseProcessCache = true;
  /// True while connect() runs its probe query: transport failures then map to 08001 / HYT01.
  std::atomic<bool> connectPhase{false};
  /// Cancel DELETEs still in flight on helper threads (drained by TrinoClient::disconnect()).
  std::mutex cancelMutex;
  std::condition_variable cancelCv;
  int pendingCancels = 0;
};

/// Waits up to `ms` for every cancel DELETE sent through `cs` to finish.
void drainPendingCancels(ClientShared& cs, int ms) {
  std::unique_lock<std::mutex> lk(cs.cancelMutex);
  if (cs.pendingCancels == 0) return;
  if (!cs.cancelCv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return cs.pendingCancels == 0; })) {
    TRINO_LOG_WARN(kLog, "%d cancel request(s) still pending after %d ms", cs.pendingCancels, ms);
  }
}

/// `key=urlEncode(value)` pairs joined by ',' (X-Trino-Session, X-Trino-Extra-Credential, ...).
std::string joinEncodedPairs(const std::map<std::string, std::string>& pairs) {
  std::string out;
  for (const auto& kv : pairs) {
    if (!out.empty()) out += ',';
    out += kv.first;
    out += '=';
    out += urlEncode(kv.second);
  }
  return out;
}

/// Splits `key=value` at the first '='; the key is trimmed, the value URL-decoded (Trino encodes
/// values with java.net.URLEncoder, hence '+' for spaces). False when there is no '='.
bool splitHeaderPair(std::string_view text, std::string* key, std::string* value) {
  const size_t eq = text.find('=');
  if (eq == std::string_view::npos) return false;
  *key = std::string(trim(text.substr(0, eq)));
  *value = urlDecode(trim(text.substr(eq + 1)));
  return !key->empty();
}

/// Appends `targetResultSize=<N>MB` to a nextUri, keeping whatever query string it already has.
std::string withTargetResultSize(const std::string& uri, int mb) {
  if (uri.find("targetResultSize=") != std::string::npos) return uri;
  const int clamped = std::max(1, std::min(mb, kMaxTargetResultSizeMb));
  std::string out = uri;
  const size_t fragment = out.find('#');
  std::string tail;
  if (fragment != std::string::npos) {
    tail = out.substr(fragment);
    out.erase(fragment);
  }
  out += out.find('?') == std::string::npos ? '?' : '&';
  out += "targetResultSize=" + std::to_string(clamped) + "MB";
  return out + tail;
}

/// First `n` rows of a page as a new page (SQL_ATTR_MAX_ROWS cuts inside a page).
std::unique_ptr<Page> copyLeadingRows(const Page& src, size_t n) {
  auto out = std::make_unique<Page>();
  out->columns.reserve(src.columns.size());
  for (const Column& c : src.columns) {
    Column d(c.kind());
    for (size_t r = 0; r < n; ++r) {
      if (c.isNull(r)) {
        d.appendNull();
        continue;
      }
      switch (c.kind()) {
        case StorageKind::Bool: d.appendBool(c.getBool(r)); break;
        case StorageKind::Int64: d.appendInt64(c.getInt64(r)); break;
        case StorageKind::Double: d.appendDouble(c.getDouble(r)); break;
        case StorageKind::String:
        case StorageKind::Binary: d.appendBytes(c.getBytes(r)); break;
      }
    }
    out->columns.push_back(std::move(d));
  }
  out->rowCount = n;
  return out;
}

/// Reads a bearer token from AccessTokenFile (trimmed). The file is re-read at every connect so
/// rotating tokens work without re-creating the DSN.
std::string readTokenFile(const std::string& path) {
  std::ifstream in;
#if defined(TRINO_ODBC_WINDOWS)
  // The path is UTF-8 (from the connection string); open it through the wide API so non-ANSI
  // characters work. Both MSVC and mingw's libstdc++ accept a const wchar_t* here.
  std::wstring wide;
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (wlen > 0) {
    wide.resize(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wlen);
    wide.resize(static_cast<size_t>(wlen) - 1);
  }
  if (!wide.empty()) in.open(wide.c_str(), std::ios::binary);
  else in.open(path.c_str(), std::ios::binary);
#else
  in.open(path.c_str(), std::ios::binary);
#endif
  if (!in) throw DriverError(sqlstate::kInvalidAuth, "Cannot read AccessTokenFile: " + path);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) throw DriverError(sqlstate::kInvalidAuth, "Error while reading AccessTokenFile: " + path);
  return std::string(trim(data));
}

/// Copies the connection-string defaults into a fresh SessionState (only fields that are still
/// empty). After this the session is the single source of truth for every request header, so a
/// server-side RESET SESSION / USE is never undone by a stale DSN value.
void seedSession(const ConnectionConfig& c, SessionState& s) {
  std::lock_guard<std::mutex> g(s.mutex);
  if (s.catalog.empty()) s.catalog = c.catalog;
  if (s.schema.empty()) s.schema = c.schema;
  if (s.timeZone.empty()) s.timeZone = resolveSessionTimeZone(c.timeZone);
  if (s.locale.empty()) s.locale = c.locale;
  if (s.source.empty()) s.source = c.source.empty() ? "trino-odbc" : c.source;
  if (s.clientInfo.empty()) s.clientInfo = c.clientInfo;
  if (s.traceToken.empty()) s.traceToken = c.traceToken;
  if (s.sessionUser.empty()) s.sessionUser = c.sessionUser;
  if (s.clientTags.empty()) s.clientTags = c.clientTags;
  for (const auto& kv : c.sessionProperties) s.sessionProperties.insert(kv);
  for (const auto& kv : c.extraCredentials) s.extraCredentials.insert(kv);
  for (const auto& kv : c.roles) s.roles.insert(kv);
}

/// Headers identifying the caller, sent with every request of a query (POST, GET nextUri and
/// DELETE). Trino's JDBC client sends exactly this subset on follow-up requests.
HeaderList buildIdentityHeaders(const ClientShared& cs) {
  const ConnectionConfig& c = *cs.config;
  SessionState& s = *cs.session;
  std::lock_guard<std::mutex> g(s.mutex);
  HeaderList h;
  auto add = [&h](const char* name, const std::string& value) {
    if (!value.empty()) h.emplace_back(name, value);
  };
  // SessionUser is the identity the query runs as (X-Trino-User); the authenticated identity then
  // travels as X-Trino-Original-User so the server can check the impersonation. Without a
  // session user, X-Trino-User is the configured user (may be empty for Kerberos/OAuth2/certs,
  // where the server derives it from the credential).
  if (!s.sessionUser.empty()) {
    add("X-Trino-User", s.sessionUser);
    add("X-Trino-Original-User", c.user);
  } else {
    add("X-Trino-User", c.user);
  }
  add("X-Trino-Authorization-User", s.authorizationUser);
  add("X-Trino-Source", s.source);
  add("X-Trino-Trace-Token", s.traceToken);
  add("X-Trino-Client-Tags", join(s.clientTags, ","));
  add("X-Trino-Client-Info", s.clientInfo);
  h.emplace_back("X-Trino-Client-Capabilities", kClientCapabilities);
  return h;
}

/// Full header set of `POST /v1/statement` (identity headers plus the session context).
HeaderList buildStatementHeaders(const ClientShared& cs, const ExecOptions& opts, const HeaderList& identity) {
  const ConnectionConfig& c = *cs.config;
  SessionState& s = *cs.session;
  HeaderList h = identity;
  auto add = [&h](const char* name, const std::string& value) {
    if (!value.empty()) h.emplace_back(name, value);
  };
  std::lock_guard<std::mutex> g(s.mutex);
  add("X-Trino-Catalog", s.catalog);
  add("X-Trino-Schema", s.schema);
  add("X-Trino-Path", s.path);
  add("X-Trino-Time-Zone", s.timeZone);
  add("X-Trino-Language", s.locale);
  add("X-Trino-Session", joinEncodedPairs(s.sessionProperties));
  add("X-Trino-Role", joinEncodedPairs(s.roles));
  add("X-Trino-Extra-Credential", joinEncodedPairs(s.extraCredentials));
  if (!s.preparedStatements.empty() || !opts.preparedStatements.empty()) {
    std::map<std::string, std::string> prepared = s.preparedStatements;
    for (const auto& kv : opts.preparedStatements) prepared[kv.first] = kv.second;  // per-request wins
    add("X-Trino-Prepared-Statement", joinEncodedPairs(prepared));
  }
  if (opts.transactionId.has_value()) {
    add("X-Trino-Transaction-Id", *opts.transactionId);
  } else if (opts.startTransaction) {
    h.emplace_back("X-Trino-Transaction-Id", "NONE");
  } else {
    add("X-Trino-Transaction-Id", s.transactionId);
  }
  if (c.spooling && !cs.queryDataEncoding.empty() && !opts.internal)
    h.emplace_back("X-Trino-Query-Data-Encoding", cs.queryDataEncoding);
  return h;
}

TrinoQueryError makeQueryError(const TrinoError& e, const std::string& queryId) {
  const std::string sqlstate =
      e.sqlState.empty() ? std::string(Diag::sqlstateForTrinoError(e.errorName, e.errorType)) : e.sqlState;
  return TrinoQueryError(sqlstate, e.message, e.errorCode, e.errorName, e.errorType, queryId, e.line, e.column);
}

/// `DELETE nextUri` on a detached helper thread: cancel must return immediately and must not
/// depend on the query object staying alive. Everything the helper needs is captured by value, and
/// startDetachedHelper() pins the driver module for as long as the helper runs, so a Driver Manager
/// that unloads the driver right after SQLDisconnect/SQLFreeHandle cannot unmap its code. One
/// attempt is bounded by kCancelTimeoutSec (connect included); a transport failure or a 502/503/504
/// answer is retried once. TrinoClient::disconnect() waits (bounded) for the helpers to finish.
void sendDeleteDetached(std::shared_ptr<ClientShared> cs, HeaderList headers, std::string uri) {
  {
    std::lock_guard<std::mutex> g(cs->cancelMutex);
    ++cs->pendingCancels;
  }
  const auto finished = [](ClientShared& shared) {
    {
      std::lock_guard<std::mutex> g(shared.cancelMutex);
      --shared.pendingCancels;
    }
    shared.cancelCv.notify_all();
  };
  const bool started = startDetachedHelper([cs, headers = std::move(headers), uri = std::move(uri), finished] {
    struct Done {
      ClientShared& shared;
      void (*fn)(ClientShared&);
      ~Done() { fn(shared); }
    } done{*cs, finished};
    // Logged redacted, like every URL the transport logs.
    const std::string logged = redactUrlForLog(uri, false);
    for (int attempt = 1; attempt <= kCancelAttempts; ++attempt) {
      const bool last = attempt == kCancelAttempts;
      try {
        HttpClient http(cs->config, cs->share, cs->auth);
        HttpRequest req;
        req.method = HttpRequest::Method::Delete;
        req.url = uri;
        req.headers = headers;
        req.timeoutSec = kCancelTimeoutSec;
        const HttpResponse resp = http.perform(req);
        if (!last && HttpClient::isRetriableStatus(resp.status)) {
          TRINO_LOG_DEBUG(kLog, "DELETE %s -> HTTP %ld, retrying", logged.c_str(), resp.status);
        } else {
          TRINO_LOG_DEBUG(kLog, "DELETE %s -> HTTP %ld", logged.c_str(), resp.status);
          return;
        }
      } catch (const HttpError& e) {
        // Transport failure (connect refused, timeout, TLS ...): worth one more try.
        if (last) {
          TRINO_LOG_WARN(kLog, "DELETE %s failed: %s; the server ends the query at its client timeout",
                         logged.c_str(), e.what());
          return;
        }
        TRINO_LOG_DEBUG(kLog, "DELETE %s failed: %s, retrying", logged.c_str(), e.what());
      } catch (const std::exception& e) {
        TRINO_LOG_DEBUG(kLog, "DELETE %s failed: %s", logged.c_str(), e.what());
        return;
      } catch (...) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kCancelRetryDelayMs));
    }
  });
  if (!started) {
    finished(*cs);
    TRINO_LOG_WARN(kLog, "cannot start the cancel helper thread; the server query is not cancelled");
  }
}

std::string sqlForLog(const std::string& sql) {
  if (sql.size() <= kLoggedSqlLimit) return sql;
  return sql.substr(0, kLoggedSqlLimit) + "...";
}

}  // namespace client_internal

using namespace client_internal;

int64_t ConsumerActivity::nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---- session state from response headers -------------------------------------------------------

void applySessionResponseHeaders(SessionState& session, const HttpResponse& response) {
  std::lock_guard<std::mutex> g(session.mutex);
  std::string key, value;
  for (const auto& kv : response.headers) {
    const std::string& name = kv.first;
    const std::string& raw = kv.second;
    if (!istartsWith(name, "X-Trino-")) continue;
    if (iequals(name, "X-Trino-Set-Catalog")) {
      session.catalog = raw;
    } else if (iequals(name, "X-Trino-Set-Schema")) {
      session.schema = raw;
    } else if (iequals(name, "X-Trino-Set-Path")) {
      session.path = raw;
    } else if (iequals(name, "X-Trino-Set-Session")) {
      if (splitHeaderPair(raw, &key, &value)) session.sessionProperties[key] = value;
    } else if (iequals(name, "X-Trino-Clear-Session")) {
      session.sessionProperties.erase(std::string(trim(raw)));
    } else if (iequals(name, "X-Trino-Set-Role")) {
      if (splitHeaderPair(raw, &key, &value)) session.roles[key] = value;
    } else if (iequals(name, "X-Trino-Added-Prepare")) {
      if (splitHeaderPair(raw, &key, &value)) session.preparedStatements[urlDecode(key)] = value;
    } else if (iequals(name, "X-Trino-Deallocated-Prepare")) {
      session.preparedStatements.erase(urlDecode(trim(raw)));
    } else if (iequals(name, "X-Trino-Started-Transaction-Id")) {
      session.transactionId = std::string(trim(raw));
    } else if (iequals(name, "X-Trino-Clear-Transaction-Id")) {
      session.transactionId.clear();
    } else if (iequals(name, "X-Trino-Set-Authorization-User")) {
      session.authorizationUser = std::string(trim(raw));
    } else if (iequals(name, "X-Trino-Reset-Authorization-User")) {
      session.authorizationUser.clear();
    }
  }
}

// ---- QueryExecution::Impl ---------------------------------------------------------------------

struct QueryExecution::Impl {
  Impl(QueryExecution& owner, std::shared_ptr<ClientShared> shared, ExecOptions options)
      : owner(owner), cs(std::move(shared)), opts(std::move(options)) {}
  ~Impl() {
    // close() always joins; this is only a guard against a destructor reached without it.
    if (thread.joinable()) thread.join();
  }

  QueryExecution& owner;
  std::shared_ptr<ClientShared> cs;
  ExecOptions opts;
  HeaderList identityHeaders;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  bool useTargetResultSize = false;
  std::atomic<bool> cancelled{false};

  /// Transport of this query: the calling thread during the sync phase, then the producer.
  std::unique_ptr<HttpClient> http;
  /// Created (under `mutex`) by the producer on the first spooled response; never replaced.
  std::unique_ptr<SpoolFetcher> spool;

  // ---- producer-side state (sync phase on the caller, later the prefetch thread / nextPage())
  bool columnsKnown = false;
  bool syncPhase = true;
  bool stopRequested = false;   // maxRows reached, sink refused, or the queue was closed
  int64_t rowsProduced = 0;

  // ---- shared between producer and consumer, under `mutex`
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::unique_ptr<Page>> queue;
  size_t queuedBytes = 0;
  std::string nextUri;
  std::string partialCancelUri;     // last one the server sent (fallback cancel target)
  std::string lastNextUri;          // most recent non-empty nextUri (fallback cancel target)
  bool serverDone = false;          // a final response arrived (no nextUri, or an error)
  bool deleteSent = false;          // DELETE already sent (cancel, close, MAX_ROWS, failed prefetch)
  bool idleCancelled = false;       // cancelled by IdleFetchTimeout (message of the HY008)
  bool consumerWaiting = false;     // nextPage() is blocked waiting for a page
  QueryStats stats;
  /// Metadata that arrives after the sync phase is parked here and merged into the owner's
  /// fields by the consumer (the accessors in the header read them without a lock).
  std::vector<QueryWarning> pendingWarnings;
  std::string pendingUpdateType;
  std::optional<int64_t> pendingUpdateCount;
  std::exception_ptr error;
  bool closed = false;
  bool producerDone = false;
  bool threadStarted = false;
  std::thread thread;

  // ---- IdleFetchTimeout (0 = off; internal queries never use it)
  int64_t idleTimeoutMs = 0;
  std::atomic<int64_t> lastPullMs{0};   // steady ms of the last nextPage() entry / exit

  void runSync(std::string sql);
  HttpResponse postStatement(const HttpRequest& req);
  bool fetchNext(const std::string& uri);
  bool handleResponse(HttpResponse& resp);
  bool deliverPage(std::unique_ptr<Page> page);
  bool pushPage(std::unique_ptr<Page> page);
  void ensureSpool();
  void stopServerQuery();
  std::string takeCancelUriLocked();
  bool idleExpiredLocked() const;
  std::string beginIdleCancelLocked();
  void finishIdleCancel(const std::string& uri);
  std::string idleMessage() const;
  int64_t idlePollMs() const;
  void startPrefetchThread();
  void prefetchMain();
  void mergePendingLocked();
  void checkExternalCancel() const;
};

void QueryExecution::Impl::checkExternalCancel() const {
  if (cancelled.load(std::memory_order_relaxed)) throw CancelledError();
  if (opts.cancelFlag && opts.cancelFlag->load(std::memory_order_relaxed)) throw CancelledError();
}

HttpResponse QueryExecution::Impl::postStatement(const HttpRequest& req) {
  const ConnectionConfig& c = *cs->config;
  // At most two browser flows per request: one because the cached token turned out stale, one
  // fresh. A 401 after that is a genuine rejection and is reported by requireSuccess().
  for (int flows = 0;; ++flows) {
    HttpResponse resp = http->performWithRetry(req, c.maxRetries);
    if (resp.status != 401 || c.auth != AuthType::OAuth2 || flows >= 2) return resp;
    std::string challenge;
    for (const std::string& value : resp.headerValues("WWW-Authenticate")) {
      if (parseBearerChallenge(value, nullptr, nullptr)) {
        challenge = value;
        break;
      }
    }
    if (challenge.empty()) return resp;
    OAuth2Authenticator oauth(cs->oauthCacheKey, c.externalAuthTimeoutSec, c.externalAuthRedirectHandler,
                              opts.cancelFlag ? opts.cancelFlag : &cancelled);
    oauth.setUseProcessCache(cs->oauthUseProcessCache);
    // The token this request actually carried, not the connection's current one: another
    // statement may have finished a login (and published a valid token) meanwhile, which must be
    // reused rather than dropped. refreshToken() also serialises concurrent browser flows.
    if (!resp.sentBearer.empty())
      TRINO_LOG_INFO(kLog, "OAuth2 token was rejected; refreshing it");
    cs->auth->setBearer(oauth.refreshToken(*http, challenge, resp.sentBearer, cs->auth.get()));
  }
}

void QueryExecution::Impl::runSync(std::string sql) {
  http = std::make_unique<HttpClient>(cs->config, cs->share, cs->auth);
  http->setCancelFlags(&cancelled, opts.cancelFlag);
  http->setDeadline(deadline);
  http->setConnectPhase(cs->connectPhase.load());

  HttpRequest req;
  req.method = HttpRequest::Method::Post;
  req.url = cs->statementUrl;
  req.body = std::move(sql);
  req.headers = buildStatementHeaders(*cs, opts, identityHeaders);
  req.idempotent = false;  // the server may have started the query even if the reply was lost
  HttpResponse resp = postStatement(req);
  HttpClient::requireSuccess(resp, req.url);
  bool gotData = handleResponse(resp);

  // Keep following nextUri until the metadata SQLExecute must report is known: the column list
  // plus a first page for queries, the final update count for DML/DDL (updateType set).
  // The cancel state is checked after every response and before an empty nextUri is taken as
  // completion: cancel() empties nextUri, so without the check a cancel landing between two
  // requests would make an aborted query look finished (DML "succeeded", SELECT without result).
  for (;;) {
    checkExternalCancel();
    if (stopRequested) break;
    if (columnsKnown && owner.updateType_.empty() && gotData) break;
    std::string uri;
    {
      std::lock_guard<std::mutex> g(mutex);
      uri = nextUri;
    }
    if (uri.empty()) break;
    if (fetchNext(uri)) gotData = true;
  }
  checkExternalCancel();
  owner.hasResultSet_ = columnsKnown && owner.updateType_.empty();
  // Internal queries are paged by nextPage() on the calling thread inside the same ODBC call
  // (executeToMemory), so they keep honouring the caller's cancel flag (still valid then) and the
  // timeout keeps covering the whole call. For application queries, from here on only
  // cancel()/close() control the query: the statement's own flag is no longer consulted so a stale
  // value cannot abort a later fetch, and since SQL_ATTR_QUERY_TIMEOUT bounds the time SQLExecute
  // waits for the statement to execute (not how long the application takes to consume the rows),
  // the deadline is lifted for the prefetch thread and the segment downloads.
  if (!opts.internal) {
    http->setCancelFlags(&cancelled, nullptr);
    http->setDeadline(std::nullopt);
    SpoolFetcher* fetcher = nullptr;
    {
      std::lock_guard<std::mutex> g(mutex);
      deadline.reset();
      fetcher = spool.get();  // never replaced once created
    }
    if (fetcher) fetcher->setDeadline(std::nullopt);
  }
  syncPhase = false;
}

bool QueryExecution::Impl::fetchNext(const std::string& uri) {
  HttpRequest req;
  req.method = HttpRequest::Method::Get;
  req.url = useTargetResultSize ? withTargetResultSize(uri, cs->config->targetResultSizeMb) : uri;
  req.headers = identityHeaders;
  req.idempotent = true;
  HttpResponse resp = http->performWithRetry(req, cs->config->maxRetries);
  HttpClient::requireSuccess(resp, uri);
  return handleResponse(resp);
}

bool QueryExecution::Impl::handleResponse(HttpResponse& resp) {
  StatementResponse r = parseStatementResponse(resp.body, columnsKnown ? &owner.columns_ : nullptr, cs->typeMapping);
  applySessionResponseHeaders(*cs->session, resp);

  if (r.columnsPresent && !columnsKnown) {
    if (!syncPhase) {
      // Cannot happen with Trino (columns precede data and the sync phase waits for them); the
      // consumer may already be reading columns_, so refuse instead of racing.
      throw DriverError(sqlstate::kGeneralError, "Column metadata arrived after the result set was opened");
    }
    owner.columns_ = std::move(r.columns);
    columnsKnown = true;
  }
  {
    std::lock_guard<std::mutex> g(mutex);
    if (syncPhase) {
      if (owner.queryId_.empty()) owner.queryId_ = r.id;
      if (owner.infoUri_.empty()) owner.infoUri_ = r.infoUri;
      for (QueryWarning& w : r.warnings) owner.warnings_.push_back(std::move(w));
      if (!r.updateType.empty()) owner.updateType_ = r.updateType;
      if (r.updateCount) owner.updateCount_ = r.updateCount;
    } else {
      for (QueryWarning& w : r.warnings) pendingWarnings.push_back(std::move(w));
      if (!r.updateType.empty()) pendingUpdateType = r.updateType;
      if (r.updateCount) pendingUpdateCount = r.updateCount;
    }
    if (r.statsPresent) stats = r.stats;
    if (!r.partialCancelUri.empty()) partialCancelUri = r.partialCancelUri;
    // A failed query has nothing more to fetch (and must not be fetched further). A response that
    // arrives after the query was cancelled does not revive nextUri: the producer loops stop there.
    const bool final = r.error.has_value() || r.nextUri.empty();
    if (final) serverDone = true;
    else lastNextUri = r.nextUri;
    nextUri = final || deleteSent ? std::string() : r.nextUri;
  }
  if (r.error) {
    TRINO_LOG_INFO(kLog, "query %s failed: %s (%s/%s, code %d)", r.id.c_str(), r.error->message.c_str(),
                   r.error->errorName.c_str(), r.error->errorType.c_str(), r.error->errorCode);
    throw makeQueryError(*r.error, r.id.empty() ? owner.queryId_ : r.id);
  }

  bool gotData = false;
  if (r.page) {
    // Older servers report the DML row count only through the single `rows` column.
    if (syncPhase && !owner.updateType_.empty() && !owner.updateCount_ && r.page->rowCount == 1 &&
        owner.columns_.size() == 1 && iequals(owner.columns_[0].name, "rows") &&
        r.page->columns[0].kind() == StorageKind::Int64 && !r.page->columns[0].isNull(0)) {
      owner.updateCount_ = r.page->columns[0].getInt64(0);
    }
    gotData = true;
    if (!deliverPage(std::move(r.page))) return gotData;
  }
  if (r.dataIsSpooled && !r.spooled.segments.empty()) {
    gotData = true;
    ensureSpool();
    SpooledData data = std::move(r.spooled);
    // During the sync phase SQLCancel can only reach this query through the statement's flag
    // (no QueryExecution has been handed out yet), so the segment wait polls it too.
    const bool more = spool->process(std::move(data), owner.columns_,
                                     [this](std::unique_ptr<Page> page) { return deliverPage(std::move(page)); },
                                     syncPhase ? opts.cancelFlag : nullptr);
    if (!more) stopRequested = true;
  }
  return gotData;
}

bool QueryExecution::Impl::deliverPage(std::unique_ptr<Page> page) {
  if (stopRequested) return false;
  if (!page || page->rowCount == 0) return true;
  if (opts.maxRows > 0) {
    const int64_t remaining = opts.maxRows - rowsProduced;
    if (remaining <= 0) {
      stopRequested = true;
      return false;
    }
    if (static_cast<int64_t>(page->rowCount) > remaining) page = copyLeadingRows(*page, static_cast<size_t>(remaining));
  }
  rowsProduced += static_cast<int64_t>(page->rowCount);
  if (!pushPage(std::move(page))) {
    stopRequested = true;
    return false;
  }
  if (opts.maxRows > 0 && rowsProduced >= opts.maxRows) {
    TRINO_LOG_DEBUG(kLog, "query %s: SQL_ATTR_MAX_ROWS (%lld) reached, stopping the fetch", owner.queryId_.c_str(),
                    static_cast<long long>(opts.maxRows));
    stopServerQuery();
    stopRequested = true;
    return false;
  }
  return true;
}

bool QueryExecution::Impl::pushPage(std::unique_ptr<Page> page) {
  const size_t bytes = page->byteSize();
  const size_t maxPages = static_cast<size_t>(std::max(1, cs->config->prefetchPages));
  const size_t maxBytes = static_cast<size_t>(std::max<int64_t>(1, cs->config->maxPrefetchBytes));
  std::unique_lock<std::mutex> lk(mutex);
  // Only a real producer thread waits for room: during the sync phase and for internal queries
  // the pusher is the consumer itself, so blocking would never end. A page always fits into an
  // empty queue, whatever its size.
  if (threadStarted) {
    const auto ready = [&] {
      return closed || cancelled.load(std::memory_order_relaxed) || queue.empty() ||
             (queue.size() < maxPages && queuedBytes < maxBytes);
    };
    if (idleTimeoutMs <= 0) {
      cv.wait(lk, ready);
    } else {
      // Backpressure is where an abandoned cursor leaves the producer: wake up periodically and
      // cancel the server query once the application has been away for IdleFetchTimeout.
      while (!ready()) {
        cv.wait_for(lk, std::chrono::milliseconds(idlePollMs()));
        if (!ready() && idleExpiredLocked()) {
          const std::string uri = beginIdleCancelLocked();
          lk.unlock();
          finishIdleCancel(uri);
          throw CancelledError(idleMessage());
        }
      }
    }
  }
  if (closed) return false;
  if (cancelled.load(std::memory_order_relaxed)) throw CancelledError();
  queuedBytes += bytes;
  queue.push_back(std::move(page));
  lk.unlock();
  cv.notify_all();
  return true;
}

void QueryExecution::Impl::ensureSpool() {
  std::lock_guard<std::mutex> g(mutex);
  if (spool) return;
  if (closed) throw CancelledError();
  spool = std::make_unique<SpoolFetcher>(cs->config, cs->share, &cancelled, deadline, cs->config->maxParallelSegments);
}

void QueryExecution::Impl::stopServerQuery() {
  std::string uri;
  {
    std::lock_guard<std::mutex> g(mutex);
    uri = takeCancelUriLocked();
  }
  if (!uri.empty()) sendDeleteDetached(cs, identityHeaders, uri);
}

std::string QueryExecution::Impl::takeCancelUriLocked() {
  // Emptying nextUri stops the producer loops (runSync / prefetchMain / nextPage) at their next turn.
  std::string uri;
  uri.swap(nextUri);
  if (serverDone || deleteSent) return std::string();
  // nextUri is only empty here before the first response (the POST reply is still outstanding:
  // Trino dispatches a query at the first nextUri request, so there is nothing to stop yet) or
  // after a race with another cancel path; fall back to the last URI the server announced.
  if (uri.empty()) uri = !lastNextUri.empty() ? lastNextUri : partialCancelUri;
  if (!uri.empty()) deleteSent = true;
  return uri;
}

bool QueryExecution::Impl::idleExpiredLocked() const {
  if (idleTimeoutMs <= 0 || closed || serverDone || deleteSent || consumerWaiting) return false;
  if (cancelled.load(std::memory_order_relaxed)) return false;
  int64_t last = lastPullMs.load(std::memory_order_relaxed);
  if (opts.activity) {
    if (opts.activity->inCall.load() > 0) return false;  // the application is inside the driver
    last = std::max(last, opts.activity->lastMs.load(std::memory_order_relaxed));
  }
  return ConsumerActivity::nowMs() - last >= idleTimeoutMs;
}

std::string QueryExecution::Impl::beginIdleCancelLocked() {
  idleCancelled = true;
  cancelled.store(true);
  if (opts.activity) opts.activity->cancelPending.store(true);
  return takeCancelUriLocked();
}

void QueryExecution::Impl::finishIdleCancel(const std::string& uri) {
  cv.notify_all();
  TRINO_LOG_WARN(kLog, "query %s: no application call for %lld s (IdleFetchTimeout); cancelling it",
                 owner.queryId_.c_str(), static_cast<long long>(idleTimeoutMs / 1000));
  if (!uri.empty()) sendDeleteDetached(cs, identityHeaders, uri);
}

std::string QueryExecution::Impl::idleMessage() const {
  return "Query canceled by the driver: the application did not fetch from the open result for " +
         std::to_string(idleTimeoutMs / 1000) + " s (IdleFetchTimeout)";
}

int64_t QueryExecution::Impl::idlePollMs() const {
  return std::max(kIdlePollMinMs, std::min(kIdlePollMaxMs, idleTimeoutMs / 4));
}

void QueryExecution::Impl::startPrefetchThread() {
  threadStarted = true;  // read by pushPage on the new thread; ordered by the thread creation
  try {
    thread = std::thread([this] { prefetchMain(); });
  } catch (const std::system_error& e) {
    threadStarted = false;
    throw DriverError(sqlstate::kGeneralError, std::string("Cannot start the prefetch thread: ") + e.what());
  }
}

void QueryExecution::Impl::prefetchMain() {
  // Any failure of the producer other than a cancel (which already sent DELETE) or a server-side
  // error (the query is over) leaves the server query running: stop it right away instead of
  // leaving it to Trino's client timeout, since the consumer only learns about it at its next fetch.
  bool stopServer = false;
  try {
    for (;;) {
      std::string uri;
      std::string idleUri;
      bool idle = false;
      {
        std::lock_guard<std::mutex> g(mutex);
        if (closed) break;
        if (idleExpiredLocked()) {
          idle = true;
          idleUri = beginIdleCancelLocked();
        }
        uri = nextUri;
      }
      if (idle) {
        finishIdleCancel(idleUri);
        throw CancelledError(idleMessage());
      }
      if (uri.empty() || stopRequested) break;
      if (cancelled.load(std::memory_order_relaxed)) throw CancelledError();
      fetchNext(uri);
    }
  } catch (const CancelledError& e) {
    TRINO_LOG_DEBUG(kLog, "prefetch of query %s cancelled: %s", owner.queryId_.c_str(), e.what());
    std::lock_guard<std::mutex> g(mutex);
    if (!closed) error = std::current_exception();
  } catch (const TrinoQueryError& e) {
    TRINO_LOG_DEBUG(kLog, "prefetch of query %s ended with a server error: %s", owner.queryId_.c_str(), e.what());
    std::lock_guard<std::mutex> g(mutex);
    if (!closed) error = std::current_exception();
  } catch (const DriverError& e) {
    TRINO_LOG_DEBUG(kLog, "prefetch of query %s ended with %s: %s", owner.queryId_.c_str(), e.sqlstate().c_str(),
                    e.what());
    stopServer = true;
    std::lock_guard<std::mutex> g(mutex);
    if (!closed) error = std::current_exception();
  } catch (...) {
    stopServer = true;
    std::lock_guard<std::mutex> g(mutex);
    if (!closed) error = std::current_exception();
  }
  if (stopServer) {
    try {
      stopServerQuery();
    } catch (...) {
    }
  }
  {
    std::lock_guard<std::mutex> g(mutex);
    producerDone = true;
  }
  cv.notify_all();
}

void QueryExecution::Impl::mergePendingLocked() {
  if (!pendingWarnings.empty()) {
    owner.warnings_.insert(owner.warnings_.end(), std::make_move_iterator(pendingWarnings.begin()),
                           std::make_move_iterator(pendingWarnings.end()));
    pendingWarnings.clear();
  }
  if (!pendingUpdateType.empty()) {
    owner.updateType_ = std::move(pendingUpdateType);
    pendingUpdateType.clear();
  }
  if (pendingUpdateCount) {
    owner.updateCount_ = pendingUpdateCount;
    pendingUpdateCount.reset();
  }
}

// ---- QueryExecution ---------------------------------------------------------------------------

QueryExecution::~QueryExecution() {
  try {
    close();
  } catch (...) {
    // Destructors must not throw; close() only logs anyway.
  }
}

std::unique_ptr<Page> QueryExecution::nextPage() {
  if (!impl_) return nullptr;
  Impl& im = *impl_;
  im.lastPullMs.store(ConsumerActivity::nowMs(), std::memory_order_relaxed);
  std::unique_lock<std::mutex> lk(im.mutex);
  for (;;) {
    if (im.closed) return nullptr;
    if (im.cancelled.load(std::memory_order_relaxed)) {
      if (im.idleCancelled) throw CancelledError(im.idleMessage());
      throw CancelledError();
    }
    im.mergePendingLocked();
    if (!im.queue.empty()) {
      std::unique_ptr<Page> page = std::move(im.queue.front());
      im.queue.pop_front();
      const size_t bytes = page->byteSize();
      im.queuedBytes -= std::min(im.queuedBytes, bytes);
      im.lastPullMs.store(ConsumerActivity::nowMs(), std::memory_order_relaxed);
      lk.unlock();
      im.cv.notify_all();
      return page;
    }
    if (im.error) {
      finished_.store(true);
      std::rethrow_exception(im.error);  // sticky: every later call reports the same failure
    }
    if (im.producerDone) {
      finished_.store(true);
      return nullptr;
    }
    if (!im.threadStarted) {
      // Internal query without a prefetch thread: follow nextUri on the caller's thread.
      const std::string uri = im.nextUri;
      lk.unlock();
      if (uri.empty()) {
        lk.lock();
        im.producerDone = true;
        continue;
      }
      try {
        im.fetchNext(uri);
      } catch (...) {
        lk.lock();
        im.error = std::current_exception();
        im.producerDone = true;
        continue;
      }
      lk.lock();
      if (im.nextUri.empty() || im.stopRequested) im.producerDone = true;
      continue;
    }
    im.consumerWaiting = true;
    im.cv.wait(lk);
    im.consumerWaiting = false;
    im.lastPullMs.store(ConsumerActivity::nowMs(), std::memory_order_relaxed);
  }
}

void QueryExecution::cancel() {
  if (!impl_) return;
  Impl& im = *impl_;
  std::string uri;
  {
    std::lock_guard<std::mutex> g(im.mutex);
    im.cancelled.store(true);
    uri = im.takeCancelUriLocked();
  }
  im.cv.notify_all();
  if (!uri.empty()) {
    TRINO_LOG_INFO(kLog, "cancelling query %s", queryId_.c_str());
    sendDeleteDetached(im.cs, im.identityHeaders, uri);
  }
}

bool QueryExecution::serverRunning() const {
  if (!impl_) return false;
  std::lock_guard<std::mutex> g(impl_->mutex);
  return !impl_->closed && !impl_->serverDone && !impl_->deleteSent;
}

bool QueryExecution::wasCancelled() const {
  if (!impl_) return false;
  std::lock_guard<std::mutex> g(impl_->mutex);
  return !impl_->closed && impl_->cancelled.load();
}

std::string QueryExecution::cancelMessage() const {
  if (!impl_) return CancelledError().message();
  std::lock_guard<std::mutex> g(impl_->mutex);
  return impl_->idleCancelled ? impl_->idleMessage() : CancelledError().message();
}

bool QueryExecution::cancelIfRunning() {
  if (!impl_) return false;
  Impl& im = *impl_;
  std::string uri;
  {
    std::lock_guard<std::mutex> g(im.mutex);
    if (im.closed || im.serverDone || im.deleteSent) return false;
    im.cancelled.store(true);
    uri = im.takeCancelUriLocked();
  }
  im.cv.notify_all();
  TRINO_LOG_INFO(kLog, "cancelling query %s (still running on the server)", queryId_.c_str());
  if (!uri.empty()) sendDeleteDetached(im.cs, im.identityHeaders, uri);
  return true;
}

void QueryExecution::close() {
  if (!impl_) return;
  Impl& im = *impl_;
  std::string uri;
  SpoolFetcher* spool = nullptr;
  {
    std::lock_guard<std::mutex> g(im.mutex);
    if (im.closed) return;
    // Aborts whatever transfer the producer is in; a finished query has nothing in flight.
    im.cancelled.store(true);
    uri = im.takeCancelUriLocked();
    im.closed = true;
    spool = im.spool.get();
  }
  im.cv.notify_all();
  if (spool) spool->shutdown();
  if (im.thread.joinable()) im.thread.join();
  {
    std::lock_guard<std::mutex> g(im.mutex);
    im.queue.clear();
    im.queuedBytes = 0;
    im.pendingWarnings.clear();
  }
  im.http.reset();  // releases the easy handle; the producer is gone
  if (!uri.empty()) {
    TRINO_LOG_INFO(kLog, "closing unfinished query %s: cancelling it on the server", queryId_.c_str());
    sendDeleteDetached(im.cs, im.identityHeaders, uri);
  }
  im.cs->registry->remove(this);
  TRINO_LOG_DEBUG(kLog, "query %s closed (%lld rows fetched)", queryId_.c_str(),
                  static_cast<long long>(im.rowsProduced));
}

QueryStats QueryExecution::stats() const {
  if (!impl_) return QueryStats();
  std::lock_guard<std::mutex> g(impl_->mutex);
  return impl_->stats;
}

// ---- TrinoClient ------------------------------------------------------------------------------

struct TrinoClient::Impl {
  std::shared_ptr<ClientShared> shared;
};

void TrinoClient::globalInit() { curlGlobalInit(); }

TrinoClient::TrinoClient(const ConnectionConfig& config, std::shared_ptr<SessionState> session)
    : impl_(std::make_unique<Impl>()),
      config_(config),
      session_(session ? std::move(session) : std::make_shared<SessionState>()),
      typeMapping_(config.typeMapping()),
      queryDataEncoding_(filterSupportedEncodings(config.queryDataEncoding)) {
  globalInit();
  auto cs = std::make_shared<ClientShared>();
  cs->config = std::make_shared<const ConnectionConfig>(config_);
  cs->share = std::make_shared<CurlShare>();
  cs->auth = std::make_shared<AuthContext>();
  cs->session = session_;
  cs->registry = std::make_shared<QueryRegistry>();
  cs->typeMapping = typeMapping_;
  cs->statementUrl = config_.baseUrl() + kStatementPath;
  cs->auth->setOrigin(urlOrigin(cs->statementUrl));  // credentials go to this origin only
  cs->queryDataEncoding = queryDataEncoding_;
  // The process-wide OAuth2 token cache is only shared between connections that name the same user;
  // when the identity comes solely from the browser login the token stays private to this connection.
  static std::atomic<unsigned long long> connectionSerial{0};
  cs->oauthCacheKey = config_.baseUrl() + "|" +
                      (config_.user.empty() ? "connection#" + std::to_string(++connectionSerial) : config_.user);
  cs->oauthUseProcessCache = !config_.user.empty();
  impl_->shared = std::move(cs);
  seedSession(config_, *session_);
}

TrinoClient::~TrinoClient() {
  try {
    cancelAll();
    drainPendingCancels(*impl_->shared, kCancelDrainMs);
  } catch (...) {
  }
}

void TrinoClient::connect() {
  ClientShared& cs = *impl_->shared;
  const ConnectionConfig& c = config_;

  // Credentials must never travel over plain HTTP unless the user opted in explicitly.
  if (!c.ssl) {
    if (c.auth == AuthType::Certificate)
      throw DriverError(sqlstate::kInvalidAuth, "Certificate authentication requires SSL");
    if (c.auth != AuthType::None && !c.allowInsecurePassword) {
      throw DriverError(sqlstate::kInvalidAuth,
                        std::string(authTypeName(c.auth)) +
                            " authentication requires SSL (set SSL=1, or AllowInsecurePassword=1 to send "
                            "credentials over plain HTTP)");
    }
  }

  // Fail early with an actionable message when the TLS backend cannot use the configured client
  // certificate (Schannel: PKCS#12 or certificate-store path only) instead of an opaque TLS
  // handshake error at the first request.
  if (!c.clientCert.empty()) checkClientCertificateSupport(c, curlTlsBackend());

  // Kerberos: libcurl derives the SPNEGO service principal from the request URL's host
  // (<KerberosServiceName>/<URL host>) and offers no separate option for it (CURLOPT_CONNECT_TO
  // changes only the connect target, not the SPN). KerberosUseCanonicalHostname=1 (the default,
  // as in Trino JDBC) therefore sends the requests to the canonical DNS name of Host — the target
  // of its CNAME chain — so that SSPI (which never canonicalises) asks for the principal the
  // coordinator is registered under. The TLS certificate must then cover the canonical name.
  // With 0 the host is used exactly as given; on Unix, MIT/Heimdal GSS-API may still canonicalise
  // on its own according to krb5.conf (dns_canonicalize_hostname / rdns).
  std::string statementUrl = c.baseUrl() + kStatementPath;
  if (c.auth == AuthType::Kerberos && c.kerberosUseCanonicalHostname) {
    const std::string canonical = canonicalHostName(c.host);
    if (!canonical.empty() && !iequals(canonical, c.host)) {
      TRINO_LOG_INFO(kLog, "Kerberos: using canonical host name %s for %s (KerberosUseCanonicalHostname=1)",
                     canonical.c_str(), c.host.c_str());
      statementUrl = replaceUrlHost(statementUrl, canonical);
    }
  }
  cs.statementUrl = statementUrl;
  cs.auth->setOrigin(urlOrigin(statementUrl));

  switch (c.auth) {
    case AuthType::Jwt: {
      std::string token(trim(c.accessToken));
      if (!c.accessTokenFile.empty()) token = readTokenFile(c.accessTokenFile);
      if (token.empty()) throw DriverError(sqlstate::kInvalidAuth, "JWT authentication requires a non-empty access token");
      cs.auth->setBearer(std::move(token));
      break;
    }
    case AuthType::OAuth2: {
      OAuth2Authenticator oauth(cs.oauthCacheKey, c.externalAuthTimeoutSec, c.externalAuthRedirectHandler, nullptr);
      oauth.setUseProcessCache(cs.oauthUseProcessCache);
      std::optional<std::string> cached = oauth.cachedToken();
      cs.auth->setBearer(cached ? *cached : std::string());
      if (cached) TRINO_LOG_DEBUG(kLog, "reusing the cached OAuth2 token for %s", cs.oauthCacheKey.c_str());
      break;
    }
    default:
      cs.auth->setBearer(std::string());
      break;
  }

  cs.connectPhase.store(true);
  struct ConnectPhaseReset {
    std::atomic<bool>& flag;
    ~ConnectPhaseReset() { flag.store(false); }
  } reset{cs.connectPhase};

  // OAuth2 without a cached token needs a first request to obtain the challenge, so the browser
  // flow runs at connect time even when validation is disabled: a failed login must surface from
  // SQLDriverConnect, not from the first SQLExecute.
  const bool probe = c.validateConnection || (c.auth == AuthType::OAuth2 && cs.auth->bearer().empty());
  if (probe) {
    ExecOptions opts;
    opts.internal = true;
    std::unique_ptr<MemoryRowSource> rows = executeToMemory("SELECT version()", opts);
    serverVersion_.clear();
    if (std::unique_ptr<Page> page = rows->nextPage()) {
      const auto cells = pageToStrings(*page);
      if (!cells.empty() && !cells[0].empty() && cells[0][0]) serverVersion_ = *cells[0][0];
    }
    TRINO_LOG_INFO(kLog, "connected to %s (server version %s)", c.baseUrl().c_str(),
                   serverVersion_.empty() ? "unknown" : serverVersion_.c_str());
  } else {
    TRINO_LOG_INFO(kLog, "connection to %s prepared without validation", c.baseUrl().c_str());
  }
  connected_ = true;
}

void TrinoClient::disconnect() {
  cancelAll();
  // The cancel DELETEs run on helper threads; give them a moment so that an application that
  // exits right after disconnecting (Power BI evaluation containers do) still stops its queries.
  drainPendingCancels(*impl_->shared, kCancelDrainMs);
  connected_ = false;
  TRINO_LOG_DEBUG(kLog, "disconnected from %s", config_.baseUrl().c_str());
}

std::unique_ptr<QueryExecution> TrinoClient::execute(std::string sql, const ExecOptions& opts) {
  std::shared_ptr<ClientShared> cs = impl_->shared;
  if (!connected_ && !cs->connectPhase.load())
    throw DriverError(sqlstate::kNotConnected, "The connection is not open");

  std::unique_ptr<QueryExecution> q(new QueryExecution());
  q->impl_ = std::make_unique<QueryExecution::Impl>(*q, cs, opts);
  QueryExecution::Impl& im = *q->impl_;

  // Application statements carry SQL_ATTR_QUERY_TIMEOUT, which allocStmt already seeds from the
  // DSN's QueryTimeout, so their 0 is an explicit "no timeout" (ODBC) and must not fall back to the
  // connection default. Internal queries have no statement attribute and use the DSN value.
  const int timeoutSec =
      opts.internal && opts.queryTimeoutSec <= 0 ? config_.queryTimeoutSec : std::max(0, opts.queryTimeoutSec);
  if (timeoutSec > 0) im.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
  im.useTargetResultSize = !opts.internal;
  if (!opts.internal && config_.idleFetchTimeoutSec > 0)
    im.idleTimeoutMs = static_cast<int64_t>(config_.idleFetchTimeoutSec) * 1000;
  im.lastPullMs.store(ConsumerActivity::nowMs());
  im.identityHeaders = buildIdentityHeaders(*cs);
  cs->registry->add(q.get());

  if (TRINO_LOG_ENABLED(LogLevel::Debug))
    TRINO_LOG_DEBUG(kLog, "execute%s: %s", opts.internal ? " (internal)" : "", sqlForLog(sql).c_str());
  im.runSync(std::move(sql));  // on failure `q` is destroyed: close() sends DELETE nextUri if pending
  TRINO_LOG_DEBUG(kLog, "query %s started: columns=%zu resultSet=%d updateType=%s", q->queryId_.c_str(),
                  q->columns_.size(), q->hasResultSet_ ? 1 : 0, q->updateType_.c_str());

  {
    std::lock_guard<std::mutex> g(im.mutex);
    if (im.nextUri.empty() || im.stopRequested) {
      im.producerDone = true;
      if (!q->hasResultSet_ || im.queue.empty()) q->finished_.store(true);
    } else if (!opts.internal) {
      im.startPrefetchThread();
    }
    // Internal queries with more pages pending are driven by nextPage() on the caller's thread.
  }
  return q;
}

std::unique_ptr<MemoryRowSource> TrinoClient::executeToMemory(std::string sql, ExecOptions opts) {
  opts.internal = true;
  std::unique_ptr<QueryExecution> exec = execute(std::move(sql), opts);
  std::unique_ptr<MemoryRowSource> rows;
  try {
    rows = materialize(*exec);
  } catch (...) {
    exec->close();
    throw;
  }
  exec->close();
  return rows;
}

void TrinoClient::cancelAll() { impl_->shared->registry->cancelAll(); }

}  // namespace trino_odbc
