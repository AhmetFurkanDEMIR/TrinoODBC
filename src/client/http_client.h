#pragma once
// libcurl transport for one connection. Deliberately curl-free in the interface so that every
// other file of the client package (and their unit tests) compiles without curl headers.
//
// Threading: one HttpClient is used by one thread at a time. Several HttpClients of a connection
// share a CurlShare (DNS cache and TLS sessions only, which libcurl supports across threads; the
// connection pool stays per easy handle because libcurl does not support handing live connections
// between concurrently running threads) which is internally locked.
#include "core/config.h"
#include "util/errors.h"
#include "util/strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trino_odbc {

/// Process-wide curl_global_init (idempotent; never paired with cleanup — see CLIENT-DETAILS.md).
void curlGlobalInit();

/// Wraps a CURLSH* whose lock callbacks serialize the shared DNS cache and TLS session cache between
/// the easy handles of one connection.
class CurlShare {
 public:
  CurlShare();
  ~CurlShare();
  CurlShare(const CurlShare&) = delete;
  CurlShare& operator=(const CurlShare&) = delete;
  /// The CURLSH* (typed void* to keep curl out of this header).
  void* handle() const { return share_; }

 private:
  static void lockCallback(void* handle, int data, int access, void* userptr);
  static void unlockCallback(void* handle, int data, void* userptr);
  void* share_ = nullptr;
  std::vector<std::unique_ptr<std::mutex>> locks_;
};

/// Bearer token shared by every HttpClient of a connection (JWT, or OAuth2 obtained at runtime), and
/// the only origin (scheme://host:port) the connection's credentials may be sent to.
struct AuthContext {
  std::mutex mutex;
  std::string bearerToken;
  std::string trustedOrigin;   // urlOrigin() of the statement URL; "" = derive from config.baseUrl()
  std::string bearer() { std::lock_guard<std::mutex> g(mutex); return bearerToken; }
  void setBearer(std::string t) { std::lock_guard<std::mutex> g(mutex); bearerToken = std::move(t); }
  std::string origin() { std::lock_guard<std::mutex> g(mutex); return trustedOrigin; }
  void setOrigin(std::string o) { std::lock_guard<std::mutex> g(mutex); trustedOrigin = std::move(o); }
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
  enum class Method { Get, Post, Delete };
  Method method = Method::Get;
  std::string url;
  std::string body;                 // POST payload
  HeaderList headers;               // request-specific headers (in addition to the client's)
  bool sendAuth = true;             // apply the connection's Basic/Bearer/Negotiate credentials
  bool sendExtraHeaders = true;     // apply ConnectionConfig::extraHeaders
  int timeoutSec = -1;              // per-request override of RequestTimeout (-1 = client default, 0 = none)
  /// Whether recv/send failures may be retried (true for GET/DELETE; a POST that may already have
  /// started a query must only be retried on connect-level failures or 502/503/504).
  bool idempotent = true;
};

struct HttpResponse {
  long status = 0;
  std::string body;                 // capacity always leaves kJsonPaddingBytes spare for simdjson
  std::multimap<std::string, std::string, CaseInsensitiveLess> headers;
  std::string effectiveUrl;
  /// The bearer token this request carried ("" when none): after a 401 it identifies the token the
  /// server rejected, which may differ from the connection's current one.
  std::string sentBearer;
  /// First value of a header ("" when absent).
  std::string header(std::string_view name) const;
  std::vector<std::string> headerValues(std::string_view name) const;
  bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
 public:
  HttpClient(std::shared_ptr<const ConnectionConfig> config, std::shared_ptr<CurlShare> share,
             std::shared_ptr<AuthContext> auth);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /// Flags polled from curl's progress callback; when either becomes true the transfer aborts with
  /// CancelledError. Both may be null.
  void setCancelFlags(const std::atomic<bool>* primary, const std::atomic<bool>* secondary = nullptr);
  /// Absolute deadline after which transfers abort with HttpError(HYT00). nullopt = none.
  void setDeadline(std::optional<std::chrono::steady_clock::time_point> deadline);
  /// While true, connect-level failures map to 08001 / HYT01 instead of 08S01 / HYT00.
  void setConnectPhase(bool on) { connectPhase_ = on; }

  /// Performs one transfer. Returns whatever status the server sent (no throw on 4xx/5xx).
  /// Throws HttpError on transport failure, CancelledError when a cancel flag aborted it.
  HttpResponse perform(const HttpRequest& req);
  /// perform() with the retry policy of CLIENT-DETAILS.md (503/502/504 and transient curl errors,
  /// exponential backoff 100 ms * 2^attempt capped at 2 s plus deterministic jitter). The backoff
  /// sleep is interruptible by the cancel flags / deadline.
  HttpResponse performWithRetry(const HttpRequest& req, int maxRetries);

  /// Throws HttpError for a non-2xx response: 401/403 -> 28000, else 08S01 with a body excerpt.
  static void requireSuccess(const HttpResponse& resp, const std::string& url);
  /// True when a curl error code is transient enough to retry.
  static bool isRetriableCurlError(int curlCode, bool connectLevelOnly);
  /// True when a status is retried (502, 503, 504).
  static bool isRetriableStatus(long status) { return status == 502 || status == 503 || status == 504; }
  /// Sleeps `ms` in short slices, returning early (false) when cancelled or past the deadline.
  bool interruptibleSleep(int ms) const;
  /// Escapes a URL component (curl_easy_escape).
  static std::string escapeComponent(std::string_view s);
  /// Excerpt of a body for error messages (first 200 bytes, control characters stripped).
  static std::string bodyExcerpt(std::string_view body);

  const ConnectionConfig& config() const { return *config_; }

 private:
  struct Impl;
  static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  static size_t headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  static int progressCallback(void* clientp, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow);
  bool cancelRequested() const;
  bool deadlinePassed() const;
  void applyStaticOptions();
  [[noreturn]] void throwCurlError(int code, const std::string& url, const char* detail);

  std::shared_ptr<const ConnectionConfig> config_;
  std::shared_ptr<CurlShare> share_;
  std::shared_ptr<AuthContext> auth_;
  std::unique_ptr<Impl> impl_;
  const std::atomic<bool>* cancelPrimary_ = nullptr;
  const std::atomic<bool>* cancelSecondary_ = nullptr;
  std::optional<std::chrono::steady_clock::time_point> deadline_;
  bool connectPhase_ = false;
  unsigned jitterCounter_ = 0;
};

// ---- URL helpers --------------------------------------------------------------------------------

/// "scheme://host:port" of `url` with scheme and host lower-cased and the default port (80 / 443)
/// filled in; "" when the URL has no scheme or host. Two URLs with equal origins may share credentials.
std::string urlOrigin(std::string_view url);
/// `url` with its host replaced (port, path and query kept). IPv6 hosts are bracketed.
std::string replaceUrlHost(std::string_view url, std::string_view newHost);
/// Form of `url` that is safe to log: the query string (presigned-URL signatures, tokens) is replaced
/// by "?...", and with `pathIsSecret` the whole path too (OAuth2 token-poll and segment URIs are
/// bearer capabilities). Userinfo is always dropped.
std::string redactUrlForLog(std::string_view url, bool pathIsSecret);

// ---- TLS backend policy -----------------------------------------------------------------------------

/// Name/version of the TLS backend libcurl uses ("OpenSSL/3.3.1", "Schannel", "SecureTransport" ...).
std::string curlTlsBackend();
/// Rejects client-certificate settings the backend cannot honour, with an actionable message:
/// Schannel (the Windows build) reads CURLOPT_SSLCERT only as a PKCS#12 file or a certificate-store
/// path and ignores CURLOPT_SSLKEY, so a PEM/DER certificate or a separate ClientKey would only fail
/// later inside the TLS handshake with an opaque error. Throws DriverError (28000 for
/// AuthenticationType=Certificate, HY000 otherwise).
void checkClientCertificateSupport(const ConnectionConfig& config, std::string_view tlsBackend);
/// CURLOPT_SSL_OPTIONS revocation bits for the backend. Only Schannel checks revocation by default,
/// and fails hard when a CRL/OCSP endpoint is unreachable; it is relaxed to best effort unless the
/// connection string says `CheckCertRevocation=1` (strict) or `CheckCertRevocation=0` (no check).
long revocationSslOptions(const ConnectionConfig& config, std::string_view tlsBackend);

// ---- name resolution ----------------------------------------------------------------------------------

/// Canonical DNS name of `host` (getaddrinfo AI_CANONNAME: the target of CNAME chains). Returns ""
/// for IP literals, on resolution failure, or when the resolver reports no canonical name.
std::string canonicalHostName(const std::string& host);

// ---- helper threads ---------------------------------------------------------------------------------

/// Runs `fn` on a detached helper thread (cancel DELETEs, browser-launcher reaping) with the driver
/// module pinned, so a Driver Manager that unloads the driver right after SQLDisconnect/SQLFreeHandle
/// cannot unmap code the helper still executes:
///  * Windows: GetModuleHandleExW(FROM_ADDRESS) takes a module reference before the thread starts;
///    the thread releases it with FreeLibraryAndExitThread, so the reference lives exactly as long
///    as the helper (odbc32 and unixODBC may then unload the DLL once every helper is gone).
///  * Unix: the first helper re-opens the driver's own image (dladdr) with RTLD_NODELETE, which makes
///    every later dlclose() a no-op for the rest of the process. This is the only way to pin a
///    shared object on unixODBC (without DontDLClose) and iODBC; it costs one mapping kept after the
///    last connection.
/// Returns false (and does not run `fn`) when no thread could be started.
bool startDetachedHelper(std::function<void()> fn);

struct SessionState;
/// Applies the X-Trino-Set-* / X-Trino-Clear-* / X-Trino-Added-Prepare / X-Trino-Deallocated-Prepare /
/// X-Trino-Started-Transaction-Id / X-Trino-Clear-Transaction-Id / X-Trino-Set-Authorization-User /
/// X-Trino-Reset-Authorization-User response headers to the session (values URL-decoded where Trino
/// encodes them). Implemented in trino_client.cpp; declared here so unit tests can drive it with a
/// hand-built HttpResponse.
void applySessionResponseHeaders(SessionState& session, const HttpResponse& response);

}  // namespace trino_odbc
