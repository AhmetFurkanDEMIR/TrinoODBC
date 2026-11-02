// Connection lifecycle entry points: SQLConnect, SQLDriverConnect, SQLBrowseConnect, SQLDisconnect,
// SQLEndTran, SQLTransact (ANSI entry points + shared impl:: used by api_unicode.cpp).
// See ODBC-API-DETAILS.md "api_connect.cpp" and ARCHITECTURE.md §5.
#include "odbc/api_common.h"
#include "odbc/api_internal_attrs.h"

#include "client/trino_client.h"
#include "util/errors.h"
#include "util/strings.h"

#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef TRINO_ODBC_WINDOWS
namespace trino_odbc {
/// Setup dialog (src/platform/win/setup.cpp). Returns false when the user cancelled.
bool showConnectDialog(HWND hwnd, ConnectionConfig& cfg);
}  // namespace trino_odbc
#endif

namespace trino_odbc {

namespace {

constexpr const char* kLog = "odbc";

/// Errors raised while establishing the connection get the connect-phase SQLSTATEs the spec
/// prescribes for SQLConnect/SQLDriverConnect: 28000 stays, timeouts become HYT01, every other
/// transport failure is 08001 "Client unable to establish connection".
[[noreturn]] void rethrowAsConnectError(const HttpError& e) {
  std::string state = e.sqlstate();
  if (state == sqlstate::kTimeout || state == sqlstate::kConnectionTimeout) {
    state = sqlstate::kConnectionTimeout;
  } else if (state != sqlstate::kInvalidAuth && state != sqlstate::kServerRejectedConnection) {
    state = sqlstate::kUnableToConnect;
  }
  throw HttpError(state, e.message(), e.httpStatus(), e.url(), e.curlCode());
}

/// Seeds the session state from the configuration (the session is the single source of truth for
/// everything the client sends as X-Trino-* headers; response headers mutate it afterwards).
void initSession(Connection& c, const ConnectionConfig& cfg) {
  SessionState& ss = *c.session;
  std::lock_guard<std::mutex> lock(ss.mutex);
  ss.catalog = cfg.catalog;
  ss.schema = cfg.schema;
  ss.path.clear();
  ss.sessionProperties = cfg.sessionProperties;
  ss.roles = cfg.roles;
  ss.preparedStatements.clear();
  ss.transactionId.clear();
  ss.authorizationUser.clear();
  // Resolved here (default Europe/Istanbul, "system" = OS zone) so conversions and the
  // X-Trino-Time-Zone header agree.
  ss.timeZone = resolveSessionTimeZone(cfg.timeZone);
  ss.locale = cfg.locale;
  ss.source = cfg.source;
  ss.clientInfo = cfg.clientInfo;
  ss.traceToken = cfg.traceToken;
  ss.sessionUser = cfg.sessionUser;
  ss.clientTags = cfg.clientTags;
  ss.extraCredentials = cfg.extraCredentials;
}

/// Shared tail of every connect path: derived defaults, validation, logging, session, client.
/// `cfg` is complete (connection string / DSN / prompt already merged).
SQLRETURN establish(Connection& c, ConnectionConfig cfg) {
  if (c.connected) throw DriverError(sqlstate::kConnectionInUse, "Connection name in use: the connection is already open");
  SQLRETURN rc = SQL_SUCCESS;

  // Attributes set before connecting that feed the configuration.
  if (!c.pendingCatalog.empty()) cfg.catalog = c.pendingCatalog;
  if (c.loginTimeout > 0) cfg.connectTimeoutSec = static_cast<int>(c.loginTimeout);

  cfg.finalize();
  cfg.validate();
  Logger::global().configure(cfg.logLevel, cfg.logPath);

  for (const auto& [key, value] : cfg.unknown) {
    (void)value;
    TRINO_LOG_WARN(kLog, "unknown connection attribute ignored: %s", key.c_str());
    rc = mergeReturnCode(rc, c.diag.addWarning(sqlstate::kNoDataForAttribute,
                                               "Invalid connection string attribute ignored: " + key));
  }
  if (c.autocommit == SQL_AUTOCOMMIT_OFF && !cfg.enableTransactions) {
    // Requested before connect (when EnableTransactions was still unknown); the DSN says no.
    c.autocommit = SQL_AUTOCOMMIT_ON;
    rc = mergeReturnCode(rc, c.diag.addWarning(sqlstate::kOptionValueChanged,
                                               "SQL_ATTR_AUTOCOMMIT reset to SQL_AUTOCOMMIT_ON: transactions are not "
                                               "enabled for this data source (EnableTransactions=0)"));
  }

  c.config = cfg;
  if (c.dataSourceName.empty()) c.dataSourceName = cfg.dsn;
  c.diag.serverName = cfg.host;
  c.diag.connectionName = c.dataSourceName.empty() ? cfg.host : c.dataSourceName;
  initSession(c, cfg);

  TRINO_LOG_INFO(kLog, "connecting to %s as '%s' (auth=%s, ssl=%d, dsn='%s')", cfg.baseUrl().c_str(), cfg.user.c_str(),
                 authTypeName(cfg.auth), cfg.ssl ? 1 : 0, c.dataSourceName.c_str());
  c.client = std::make_unique<TrinoClient>(cfg, c.session);
  try {
    c.client->connect();
  } catch (const HttpError& e) {
    c.client.reset();
    rethrowAsConnectError(e);
  } catch (...) {
    c.client.reset();
    throw;
  }

  c.connected = true;
  c.serverVersion = c.client->serverVersion();
  c.inTransaction = false;
  c.pendingCatalog.clear();
  c.accessMode = cfg.readOnly ? SQL_MODE_READ_ONLY : SQL_MODE_READ_WRITE;
  c.stmtCounter = 0;
  TRINO_LOG_INFO(kLog, "connected to %s (server version '%s')", cfg.baseUrl().c_str(), c.serverVersion.c_str());
  return rc;
}

/// True when the values a prompt could supply are missing (used by SQLDriverConnect completion
/// and SQLBrowseConnect). Works on a copy so finalize() does not disturb the caller's config.
bool missingRequiredValues(const ConnectionConfig& cfg) {
  ConnectionConfig probe = cfg;
  probe.finalize();
  if (trim(probe.host).empty()) return true;
  if (probe.needsUser()) return true;
  if (probe.auth == AuthType::Ldap && (probe.user.empty() || probe.password.empty())) return true;
  return false;
}

/// Loads the DSN named in `cfg.dsn` (connection-string values win). A DSN that does not exist is
/// fatal unless the string also names the driver: `DRIVER=...;DSN=...` is a common shape produced
/// by tools that pass a nonexistent placeholder DSN along with every value inline.
void loadDsnIfNamed(ConnectionConfig& cfg) {
  const std::string dsnName = trim(cfg.dsn).empty() ? std::string() : std::string(trim(cfg.dsn));
  if (dsnName.empty()) return;
  try {
    cfg.loadDsn(dsnName, /*onlyMissing=*/true);
  } catch (const DriverError& e) {
    if (e.sqlstate() == sqlstate::kDataSourceNotFound && !trim(cfg.driver).empty()) {
      TRINO_LOG_WARN(kLog, "DSN '%s' not found; continuing with the connection string values (DRIVER given)",
                     dsnName.c_str());
      // The name must not survive into SQL_DATA_SOURCE_NAME or the SQLDriverConnect out string:
      // a driver manager given "DSN=missing;Driver=..." uses the first keyword and fails with IM002.
      cfg.dsn.clear();
      return;
    }
    throw;
  }
}

SQLRETURN endTranOnConnection(Connection& c, SQLSMALLINT completion) {
  if (completion != SQL_COMMIT && completion != SQL_ROLLBACK) {
    throw DriverError(sqlstate::kInvalidTransactionOperation,
                      "Invalid transaction operation code " + std::to_string(completion));
  }
  requireConnected(c);
  if (c.autocommit == SQL_AUTOCOMMIT_ON) {
    // Every statement was its own transaction: nothing to commit or roll back.
    return SQL_SUCCESS;
  }
  if (!c.config.enableTransactions) {
    throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                      "Transactions are not enabled for this data source (set EnableTransactions=1)");
  }
  std::string txId;
  {
    std::lock_guard<std::mutex> lock(c.session->mutex);
    txId = c.session->transactionId;
  }
  if (txId.empty() && !c.inTransaction) return SQL_SUCCESS;  // no statement started a transaction yet

  const bool commit = completion == SQL_COMMIT;
  ExecOptions opts;
  opts.internal = true;
  if (!txId.empty()) opts.transactionId = txId;
  TRINO_LOG_DEBUG(kLog, "%s transaction %s", commit ? "committing" : "rolling back", txId.c_str());
  try {
    c.client->executeToMemory(commit ? "COMMIT" : "ROLLBACK", opts);
  } catch (...) {
    if (!commit) {
      // A failed ROLLBACK leaves nothing worth keeping client-side; the server transaction is
      // abandoned and a new one starts with the next statement.
      std::lock_guard<std::mutex> lock(c.session->mutex);
      c.session->transactionId.clear();
      c.inTransaction = false;
    }
    throw;
  }
  {
    std::lock_guard<std::mutex> lock(c.session->mutex);
    c.session->transactionId.clear();
  }
  c.inTransaction = false;
  return SQL_SUCCESS;
}

SQLRETURN disconnect(Connection& c) {
  requireConnected(c);
  if (c.inTransaction) {
    throw DriverError(sqlstate::kInvalidTransactionState,
                      "Invalid transaction state: a transaction is open; call SQLEndTran before SQLDisconnect");
  }
  // Open cursors are closed and running queries cancelled; failures here must not stop the disconnect.
  c.closeAllStatements();
  // ODBC: statements and explicitly allocated descriptors still attached are freed by the driver
  // once it has disconnected (copies: the destroy calls erase from the sets).
  const std::vector<Statement*> stmts(c.statements.begin(), c.statements.end());
  for (Statement* s : stmts) c.destroyStatement(s);
  const std::vector<Descriptor*> descs(c.descriptors.begin(), c.descriptors.end());
  for (Descriptor* d : descs) c.destroyDescriptor(d);

  if (c.client) {
    try {
      c.client->disconnect();
    } catch (const std::exception& e) {
      TRINO_LOG_WARN(kLog, "disconnect reported: %s", e.what());
    }
    c.client.reset();
  }
  c.connected = false;
  c.serverVersion.clear();
  c.inTransaction = false;
  {
    std::lock_guard<std::mutex> lock(c.session->mutex);
    c.session->transactionId.clear();
    c.session->preparedStatements.clear();
    c.session->authorizationUser.clear();
  }
  TRINO_LOG_INFO(kLog, "disconnected from %s", c.config.baseUrl().c_str());
  return SQL_SUCCESS;
}

}  // namespace

namespace impl {

SQLRETURN connect(Connection& c, std::optional<std::string> dsn, std::optional<std::string> uid,
                  std::optional<std::string> pwd) {
  if (c.connected) throw DriverError(sqlstate::kConnectionInUse, "Connection name in use: the connection is already open");
  // ODBC: an empty/absent ServerName means the DEFAULT data source.
  std::string dsnName = dsn ? std::string(trim(*dsn)) : std::string();
  if (dsnName.empty()) dsnName = "DEFAULT";

  ConnectionConfig cfg;
  cfg.loadDsn(dsnName, /*onlyMissing=*/false);
  // UID/PWD arguments override the DSN values only when actually supplied.
  if (uid && !uid->empty()) {
    std::string err;
    if (!cfg.applyKeyValue("UID", *uid, &err)) throw DriverError(sqlstate::kGeneralError, err);
  }
  if (pwd && !pwd->empty()) {
    std::string err;
    if (!cfg.applyKeyValue("PWD", *pwd, &err)) throw DriverError(sqlstate::kGeneralError, err);
  }
  c.dataSourceName = dsnName;
  return establish(c, std::move(cfg));
}

SQLRETURN driverConnect(Connection& c, SQLHWND hwnd, std::string inConn, TextOut& outConn, SQLUSMALLINT completion) {
  if (c.connected) throw DriverError(sqlstate::kConnectionInUse, "Connection name in use: the connection is already open");
  switch (completion) {
    case SQL_DRIVER_NOPROMPT:
    case SQL_DRIVER_COMPLETE:
    case SQL_DRIVER_PROMPT:
    case SQL_DRIVER_COMPLETE_REQUIRED:
      break;
    default:
      throw DriverError(sqlstate::kInvalidDriverCompletion,
                        "Invalid driver completion option " + std::to_string(completion));
  }

  ConnectionConfig cfg;
  const KeyValueMap kv = ConnectionConfig::parseConnectionString(inConn);
  cfg.applyAll(kv);
  loadDsnIfNamed(cfg);
  c.dataSourceName = cfg.dsn;

  bool prompt = completion == SQL_DRIVER_PROMPT;
  if ((completion == SQL_DRIVER_COMPLETE || completion == SQL_DRIVER_COMPLETE_REQUIRED) && missingRequiredValues(cfg)) {
    prompt = true;
  }
  if (prompt && hwnd == nullptr) {
    // ODBC: a null WindowHandle means no dialog boxes. An unowned modal dialog in a service or
    // session-0 process would block forever, so behave like SQL_DRIVER_NOPROMPT instead.
    TRINO_LOG_DEBUG(kLog, "no window handle: connection dialog suppressed (completion=%u)",
                    static_cast<unsigned>(completion));
    prompt = false;
  }
  if (prompt) {
#ifdef TRINO_ODBC_WINDOWS
    TRINO_LOG_DEBUG(kLog, "showing the connection dialog (completion=%u)", static_cast<unsigned>(completion));
    if (!showConnectDialog(hwnd, cfg)) {
      TRINO_LOG_INFO(kLog, "connection dialog cancelled by the user");
      return SQL_NO_DATA;
    }
#else
    // No dialog on this platform: behave like SQL_DRIVER_NOPROMPT (validation reports what is
    // missing). IM007 is the spec's answer when nothing usable was given at all.
    (void)hwnd;
    if (trim(cfg.host).empty()) {
      throw DriverError(sqlstate::kNoDataSourceOrDriver,
                        "No data source or driver specified; dialog prohibited: Host (or a DSN providing it) is required");
    }
#endif
  } else if (trim(cfg.host).empty()) {
    throw DriverError(sqlstate::kNoDataSourceOrDriver,
                      "No data source or driver specified; dialog prohibited: Host (or a DSN providing it) is required");
  }

  SQLRETURN rc = establish(c, std::move(cfg));
  // The out string must let the application reconnect without prompting, so it carries every
  // value including the password exactly as given (never the masked form used for logging).
  rc = mergeReturnCode(rc, outConn.writeWithDiag(c.config.toConnectionString(/*includeSecrets=*/true), c.diag));
  return rc;
}

SQLRETURN browseConnect(Connection& c, std::string inConn, TextOut& outConn) {
  if (c.connected) throw DriverError(sqlstate::kConnectionInUse, "Connection name in use: the connection is already open");
  // The browse dialogue spans several calls; the connection's config accumulates the attributes
  // supplied so far (it is replaced by the final one when the connection is established).
  ConnectionConfig& acc = c.config;
  try {
    const KeyValueMap kv = ConnectionConfig::parseConnectionString(inConn);
    acc.applyAll(kv);
    loadDsnIfNamed(acc);
    if (!acc.dsn.empty()) c.dataSourceName = acc.dsn;

    if (missingRequiredValues(acc)) {
      ConnectionConfig probe = acc;
      probe.finalize();
      // Browse result grammar: a leading '*' marks an optional keyword; unprefixed keywords are the
      // ones missingRequiredValues() insists on.
      std::vector<std::string> needed;
      if (trim(probe.host).empty()) needed.emplace_back("Host:Host=?");
      if (probe.port <= 0) needed.emplace_back("*Port:Port=?");
      if (probe.needsUser()) needed.emplace_back("UID:User ID=?");
      else if (probe.user.empty()) needed.emplace_back("*UID:User ID=?");
      if (probe.password.empty()) {
        // Required for LDAP; otherwise only offered while the auth type is still implicit (a
        // password would select LDAP), never for an explicitly password-less mode.
        if (probe.auth == AuthType::Ldap) needed.emplace_back("PWD:Password=?");
        else if (!probe.authExplicit) needed.emplace_back("*PWD:Password=?");
      }
      if (probe.catalog.empty()) needed.emplace_back("*Catalog:Catalog=?");
      if (probe.schema.empty()) needed.emplace_back("*Schema:Schema=?");
      if (!probe.sslExplicit) needed.emplace_back("*SSL:SSL=?");
      outConn.write(join(needed, ";"));
      TRINO_LOG_DEBUG(kLog, "SQLBrowseConnect needs more attributes: %s", join(needed, ";").c_str());
      return SQL_NEED_DATA;
    }
  } catch (...) {
    acc = ConnectionConfig();  // a failed step ends the browse dialogue
    throw;
  }

  ConnectionConfig cfg = acc;
  try {
    SQLRETURN rc = establish(c, std::move(cfg));
    rc = mergeReturnCode(rc, outConn.writeWithDiag(c.config.toConnectionString(/*includeSecrets=*/true), c.diag));
    return rc;
  } catch (...) {
    acc = ConnectionConfig();
    throw;
  }
}

SQLRETURN endTransaction(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT completion) {
  if (handleType == SQL_HANDLE_DBC) {
    return guardDbc(handle, "SQLEndTran", [&](Connection& c) -> SQLRETURN { return endTranOnConnection(c, completion); });
  }
  if (handleType == SQL_HANDLE_ENV) {
    return guardEnv(handle, "SQLEndTran", [&](Environment& env) -> SQLRETURN {
      if (completion != SQL_COMMIT && completion != SQL_ROLLBACK) {
        throw DriverError(sqlstate::kInvalidTransactionOperation,
                          "Invalid transaction operation code " + std::to_string(completion));
      }
      // Environment before connection (same order as SQLFreeHandle); every open connection is
      // completed, failures are recorded on both the connection and the environment.
      SQLRETURN rc = SQL_SUCCESS;
      for (Connection* c : env.connections) {
        HandleLock lock(c->mutex);
        if (!c->connected) continue;
        c->diag.clear();
        try {
          rc = mergeReturnCode(rc, endTranOnConnection(*c, completion));
        } catch (const std::exception& e) {
          c->diag.returnCode = diagFromException(c->diag, e);
          rc = mergeReturnCode(rc, diagFromException(env.diag, e));
        }
      }
      return rc;
    });
  }
  return SQL_INVALID_HANDLE;
}

void resetPooledConnection(Connection& c) {
  TRINO_LOG_DEBUG(kLog, "SQL_ATTR_RESET_CONNECTION: restoring connection defaults for pool re-use");
  // Cursors are closed and running queries cancelled; the next user starts with idle statements.
  c.closeAllStatements();
  if (c.connected && c.client) {
    std::string txId;
    {
      std::lock_guard<std::mutex> lock(c.session->mutex);
      txId = c.session->transactionId;
    }
    if (c.inTransaction || !txId.empty()) {
      // Work of the previous user must never be committed on behalf of the next one.
      ExecOptions opts;
      opts.internal = true;
      if (!txId.empty()) opts.transactionId = txId;
      try {
        c.client->executeToMemory("ROLLBACK", opts);
      } catch (const std::exception& e) {
        TRINO_LOG_WARN(kLog, "rollback during connection reset failed: %s", e.what());
      }
    }
  }
  c.inTransaction = false;
  // Catalog, schema, path, session properties, roles, prepared statements, transaction id and
  // authorization user all live in the client-side session (sent as X-Trino-* headers), so
  // re-seeding it from the configuration restores the server-visible state as well.
  initSession(c, c.config);
  c.pendingCatalog.clear();
  c.autocommit = SQL_AUTOCOMMIT_ON;
  c.accessMode = c.config.readOnly ? SQL_MODE_READ_ONLY : SQL_MODE_READ_WRITE;
  c.txnIsolation = SQL_TXN_READ_UNCOMMITTED;
  c.metadataId = SQL_FALSE;
  c.connectionTimeout = 0;
  c.packetSize = 0;
  c.asyncEnable = SQL_ASYNC_ENABLE_OFF;
}

}  // namespace impl
}  // namespace trino_odbc

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLConnect(SQLHDBC ConnectionHandle, SQLCHAR* ServerName, SQLSMALLINT NameLength1,
                                            SQLCHAR* UserName, SQLSMALLINT NameLength2, SQLCHAR* Authentication,
                                            SQLSMALLINT NameLength3) {
  return guardDbc(ConnectionHandle, "SQLConnect", [&](Connection& c) -> SQLRETURN {
    const AnsiEncoding enc = ansiEncodingOf(c);
    return impl::connect(c, textIn(ServerName, NameLength1, enc), textIn(UserName, NameLength2, enc),
                         textIn(Authentication, NameLength3, enc));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLDriverConnect(SQLHDBC hdbc, SQLHWND hwnd, SQLCHAR* szConnStrIn, SQLSMALLINT cbConnStrIn,
                                                  SQLCHAR* szConnStrOut, SQLSMALLINT cbConnStrOutMax,
                                                  SQLSMALLINT* pcbConnStrOut, SQLUSMALLINT fDriverCompletion) {
  return guardDbc(hdbc, "SQLDriverConnect", [&](Connection& c) -> SQLRETURN {
    const AnsiEncoding enc = ansiEncodingOf(c);
    if (cbConnStrOutMax < 0) throw DriverError(sqlstate::kInvalidStringOrBufferLength, "BufferLength must not be negative");
    TextOut out(szConnStrOut, cbConnStrOutMax, pcbConnStrOut, enc);
    return impl::driverConnect(c, hwnd, textIn(szConnStrIn, cbConnStrIn, enc).value_or(std::string()), out,
                               fDriverCompletion);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLBrowseConnect(SQLHDBC hdbc, SQLCHAR* szConnStrIn, SQLSMALLINT cbConnStrIn,
                                                  SQLCHAR* szConnStrOut, SQLSMALLINT cbConnStrOutMax,
                                                  SQLSMALLINT* pcbConnStrOut) {
  return guardDbc(hdbc, "SQLBrowseConnect", [&](Connection& c) -> SQLRETURN {
    const AnsiEncoding enc = ansiEncodingOf(c);
    if (cbConnStrOutMax < 0) throw DriverError(sqlstate::kInvalidStringOrBufferLength, "BufferLength must not be negative");
    TextOut out(szConnStrOut, cbConnStrOutMax, pcbConnStrOut, enc);
    return impl::browseConnect(c, textIn(szConnStrIn, cbConnStrIn, enc).value_or(std::string()), out);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLDisconnect(SQLHDBC ConnectionHandle) {
  return guardDbc(ConnectionHandle, "SQLDisconnect", [&](Connection& c) -> SQLRETURN { return disconnect(c); });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT CompletionType) {
  return impl::endTransaction(HandleType, Handle, CompletionType);
}

TRINO_ODBC_API SQLRETURN SQL_API SQLTransact(SQLHENV EnvironmentHandle, SQLHDBC ConnectionHandle, SQLUSMALLINT CompletionType) {
  const SQLSMALLINT completion = static_cast<SQLSMALLINT>(CompletionType);
  // Internal helper, not the exported SQLEndTran: on Linux that name may bind to the driver manager.
  if (ConnectionHandle != SQL_NULL_HDBC) return impl::endTransaction(SQL_HANDLE_DBC, ConnectionHandle, completion);
  return impl::endTransaction(SQL_HANDLE_ENV, EnvironmentHandle, completion);
}

}  // extern "C"
