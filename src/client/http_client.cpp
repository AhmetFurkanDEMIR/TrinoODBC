// libcurl transport. See http_client.h for the threading model and CLIENT-DETAILS.md for the
// option matrix this file implements.
#include "client/http_client.h"

#include "result/json_decoder.h"  // ensureJsonPadding / kJsonPaddingBytes
#include "util/log.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#if defined(TRINO_ODBC_WINDOWS)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#elif defined(TRINO_ODBC_UNIX)
#  include <dlfcn.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#endif

namespace trino_odbc {

namespace {
constexpr const char* kLog = "http";
constexpr int kSleepSliceMs = 50;

std::string hostOf(const std::string& url) {
  const size_t scheme = url.find("://");
  const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t end = url.find_first_of("/?#", start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

const char* methodName(HttpRequest::Method m) {
  switch (m) {
    case HttpRequest::Method::Get: return "GET";
    case HttpRequest::Method::Post: return "POST";
    case HttpRequest::Method::Delete: return "DELETE";
  }
  return "?";
}

/// RAII for curl_slist.
struct SList {
  curl_slist* list = nullptr;
  ~SList() { curl_slist_free_all(list); }
  void add(const std::string& line) {
    curl_slist* next = curl_slist_append(list, line.c_str());
    if (!next) throw DriverError(sqlstate::kMemoryAllocation, "curl_slist_append failed");
    list = next;
  }
};

/// Pieces of an absolute URL, as offsets into the original text.
struct UrlParts {
  std::string_view scheme;
  size_t hostBegin = 0, hostEnd = 0;   // host including IPv6 brackets
  std::string_view host;               // without brackets
  std::string_view port;
  size_t pathBegin = 0;                // start of path (or of '?'/'#', or end)
  size_t queryBegin = std::string_view::npos;
};

bool parseUrl(std::string_view url, UrlParts* out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0) return false;
  UrlParts p;
  p.scheme = url.substr(0, schemeEnd);
  const size_t authBegin = schemeEnd + 3;
  size_t authEnd = url.find_first_of("/?#", authBegin);
  if (authEnd == std::string_view::npos) authEnd = url.size();
  const std::string_view authority = url.substr(authBegin, authEnd - authBegin);
  const size_t at = authority.rfind('@');
  const size_t hostPortBegin = at == std::string_view::npos ? authBegin : authBegin + at + 1;
  const std::string_view hostPort = url.substr(hostPortBegin, authEnd - hostPortBegin);
  if (!hostPort.empty() && hostPort.front() == '[') {
    const size_t close = hostPort.find(']');
    if (close == std::string_view::npos) return false;
    p.host = hostPort.substr(1, close - 1);
    p.hostBegin = hostPortBegin;
    p.hostEnd = hostPortBegin + close + 1;
    if (close + 1 < hostPort.size()) {
      if (hostPort[close + 1] != ':') return false;
      p.port = hostPort.substr(close + 2);
    }
  } else {
    const size_t colon = hostPort.rfind(':');
    p.host = colon == std::string_view::npos ? hostPort : hostPort.substr(0, colon);
    p.hostBegin = hostPortBegin;
    p.hostEnd = hostPortBegin + p.host.size();
    if (colon != std::string_view::npos) p.port = hostPort.substr(colon + 1);
  }
  if (p.host.empty()) return false;
  p.pathBegin = authEnd;
  const size_t q = url.find_first_of("?#", authEnd);
  if (q != std::string_view::npos) p.queryBegin = q;
  *out = p;
  return true;
}

bool isSchannel(std::string_view backend) { return istartsWith(backend, "Schannel"); }

bool isPkcs12Path(const std::string& path) { return iendsWith(path, ".p12") || iendsWith(path, ".pfx"); }

/// Schannel certificate-store reference: "<location>\<store>\<thumbprint>" (libcurl's
/// get_cert_location() accepts exactly these location names).
bool isCertStorePath(const std::string& path) {
  static const char* const kLocations[] = {"CurrentUser",           "LocalMachine",           "CurrentService",
                                           "Services",              "Users",                  "CurrentUserGroupPolicy",
                                           "LocalMachineGroupPolicy", "LocalMachineEnterprise"};
  for (const char* loc : kLocations) {
    const size_t n = std::strlen(loc);
    if (path.size() > n + 1 && istartsWith(path, loc) && path[n] == '\\') return true;
  }
  return false;
}

/// Reads a whole (small) file; the path is UTF-8 and is opened through the wide API on Windows so
/// non-ANSI characters work. False when it cannot be read.
bool readWholeFile(const std::string& path, std::string* out) {
  std::ifstream in;
#if defined(TRINO_ODBC_WINDOWS)
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
  if (!in) return false;
  out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return !in.bad();
}

#if defined(TRINO_ODBC_WINDOWS)
/// Thread start block of startDetachedHelper (owned by the thread once it runs).
struct HelperStart {
  std::function<void()> fn;
  HMODULE module = nullptr;
};

DWORD WINAPI helperThreadProc(LPVOID param) {
  HMODULE module = nullptr;
  {
    std::unique_ptr<HelperStart> start(static_cast<HelperStart*>(param));
    module = start->module;
    try {
      start->fn();
    } catch (...) {
      // Helpers log their own failures; nothing may escape a thread procedure.
    }
    // `start` (and whatever the helper captured) is destroyed here, while the module is pinned.
  }
  // Drops the reference taken by startDetachedHelper and exits without returning into code that
  // may be unmapped by then.
  if (module) FreeLibraryAndExitThread(module, 0);
  return 0;
}
#elif defined(TRINO_ODBC_UNIX)
/// Anchor whose address identifies the driver's own image for dladdr().
const char kModuleAnchor = 0;

void pinModuleForProcessLifetime() {
  static std::once_flag once;
  std::call_once(once, [] {
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (dladdr(&kModuleAnchor, &info) == 0 || info.dli_fname == nullptr) {
      TRINO_LOG_DEBUG(kLog, "dladdr failed; the driver image is not pinned");
      return;
    }
    // Already loaded, so this only adds a reference and the NODELETE flag; the handle is
    // intentionally never closed. Fails harmlessly when the driver is linked into an executable.
    void* self = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NODELETE);
    if (!self) TRINO_LOG_DEBUG(kLog, "could not pin %s (%s)", info.dli_fname, dlerror());
  });
}
#endif
}  // namespace

// ---- URL helpers --------------------------------------------------------------------------------

std::string urlOrigin(std::string_view url) {
  UrlParts p;
  if (!parseUrl(url, &p)) return std::string();
  const std::string scheme = toLower(p.scheme);
  std::string port(p.port);
  if (port.empty()) {
    if (scheme == "https") port = "443";
    else if (scheme == "http") port = "80";
  }
  std::string out = scheme + "://";
  const std::string host = toLower(p.host);
  if (host.find(':') != std::string::npos) out += "[" + host + "]";
  else out += host;
  if (!port.empty()) out += ":" + port;
  return out;
}

std::string replaceUrlHost(std::string_view url, std::string_view newHost) {
  UrlParts p;
  if (!parseUrl(url, &p) || newHost.empty()) return std::string(url);
  std::string host(newHost);
  if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";
  return std::string(url.substr(0, p.hostBegin)) + host + std::string(url.substr(p.hostEnd));
}

std::string redactUrlForLog(std::string_view url, bool pathIsSecret) {
  UrlParts p;
  if (!parseUrl(url, &p)) return "<unparsable URL>";
  std::string out = std::string(p.scheme) + "://" + std::string(url.substr(p.hostBegin, p.pathBegin - p.hostBegin));
  const size_t pathEnd = p.queryBegin == std::string_view::npos ? url.size() : p.queryBegin;
  const std::string_view path = url.substr(p.pathBegin, pathEnd - p.pathBegin);
  if (pathIsSecret) {
    if (!path.empty() && path != "/") out += "/<redacted>";
  } else {
    out += std::string(path);
  }
  if (p.queryBegin != std::string_view::npos && url[p.queryBegin] == '?') out += "?...";
  return out;
}

// ---- TLS backend policy -----------------------------------------------------------------------

std::string curlTlsBackend() {
  curlGlobalInit();
  const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
  return v && v->ssl_version ? std::string(v->ssl_version) : std::string();
}

void checkClientCertificateSupport(const ConnectionConfig& c, std::string_view tlsBackend) {
  if (c.clientCert.empty() || !isSchannel(tlsBackend)) return;
  if (isPkcs12Path(c.clientCert) || isCertStorePath(c.clientCert)) {
    if (!c.clientKey.empty())
      TRINO_LOG_WARN(kLog, "ClientKey is ignored by the Windows TLS backend (Schannel): the private key must be "
                           "inside the PKCS#12 file or the certificate store entry named by ClientCert");
    return;
  }
  const char* state = c.auth == AuthType::Certificate ? sqlstate::kInvalidAuth : sqlstate::kGeneralError;
  throw DriverError(state,
                    "ClientCert '" + c.clientCert +
                        "' cannot be used by the Windows TLS backend (Schannel), which reads neither PEM nor DER "
                        "certificates nor a separate ClientKey: convert the certificate and its key into one "
                        "PKCS#12 file (for example: openssl pkcs12 -export -in client.pem -inkey client.key -out "
                        "client.pfx) and set ClientCert to the .pfx/.p12 file, or use a certificate-store path "
                        "such as CurrentUser\\MY\\<thumbprint>");
}

long revocationSslOptions(const ConnectionConfig& c, std::string_view tlsBackend) {
  if (!isSchannel(tlsBackend)) return 0;  // OpenSSL / SecureTransport: no revocation check by default
  // CheckCertRevocation is the Simba/Starburst key; it is read from the unrecognised-key bucket so
  // DSNs written for those drivers keep their meaning.
  auto it = c.unknown.find("CheckCertRevocation");
  if (it != c.unknown.end()) {
    bool strict = false;
    if (parseBool(it->second, &strict)) {
#if defined(CURLSSLOPT_NO_REVOKE)
      if (!strict) return CURLSSLOPT_NO_REVOKE;
#endif
      if (strict) return 0;  // Schannel's own default: fail when revocation cannot be checked
    } else {
      TRINO_LOG_WARN(kLog, "Ignoring CheckCertRevocation='%s' (expected 0 or 1)", it->second.c_str());
    }
  }
#if defined(CURLSSLOPT_REVOKE_BEST_EFFORT)
  // Internal PKIs often publish CRL/OCSP endpoints the client cannot reach; best effort still
  // rejects a certificate that is known to be revoked.
  return CURLSSLOPT_REVOKE_BEST_EFFORT;
#else
  return 0;
#endif
}

// ---- name resolution ----------------------------------------------------------------------------

std::string canonicalHostName(const std::string& host) {
  if (host.empty()) return std::string();
  curlGlobalInit();  // on Windows this also runs WSAStartup, which getaddrinfo needs
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
    freeaddrinfo(res);
    return std::string();  // IP literal: nothing to canonicalise
  }
  res = nullptr;
  hints.ai_flags = AI_CANONNAME;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return std::string();
  std::string canonical = res->ai_canonname ? std::string(res->ai_canonname) : std::string();
  freeaddrinfo(res);
  while (!canonical.empty() && canonical.back() == '.') canonical.pop_back();
  return canonical;
}

// ---- helper threads -----------------------------------------------------------------------------

bool startDetachedHelper(std::function<void()> fn) {
#if defined(TRINO_ODBC_WINDOWS)
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&helperThreadProc),
                          &module)) {
    TRINO_LOG_WARN(kLog, "GetModuleHandleExW failed (%lu); helper thread runs unpinned",
                   static_cast<unsigned long>(GetLastError()));
    module = nullptr;
  }
  auto start = std::make_unique<HelperStart>();
  start->fn = std::move(fn);
  start->module = module;
  HANDLE thread = CreateThread(nullptr, 0, &helperThreadProc, start.get(), 0, nullptr);
  if (!thread) {
    TRINO_LOG_WARN(kLog, "cannot start a helper thread (%lu)", static_cast<unsigned long>(GetLastError()));
    if (module) FreeLibrary(module);
    return false;
  }
  start.release();  // owned by the thread now
  CloseHandle(thread);
  return true;
#else
#  if defined(TRINO_ODBC_UNIX)
  pinModuleForProcessLifetime();
#  endif
  try {
    std::thread(std::move(fn)).detach();
    return true;
  } catch (const std::exception& e) {
    TRINO_LOG_WARN(kLog, "cannot start a helper thread: %s", e.what());
    return false;
  }
#endif
}

// ---- global init ----------------------------------------------------------------------------

void curlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    const CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
    if (rc != CURLE_OK) TRINO_LOG_ERROR(kLog, "curl_global_init failed: %s", curl_easy_strerror(rc));
  });
}

// ---- CurlShare --------------------------------------------------------------------------------

CurlShare::CurlShare() {
  curlGlobalInit();
  for (int i = 0; i < static_cast<int>(CURL_LOCK_DATA_LAST); ++i) locks_.push_back(std::make_unique<std::mutex>());
  CURLSH* sh = curl_share_init();
  if (!sh) throw DriverError(sqlstate::kMemoryAllocation, "curl_share_init failed");
  share_ = sh;
  curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, &CurlShare::lockCallback);
  curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, &CurlShare::unlockCallback);
  curl_share_setopt(sh, CURLSHOPT_USERDATA, this);
  curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  // CURL_LOCK_DATA_CONNECT is deliberately NOT shared: libcurl documents that sharing the connection
  // pool between concurrently running threads is unsupported (the lock callbacks only protect the
  // cache itself, not a live TLS / HTTP/2 connection handed to another thread). Every HttpClient
  // lives for a whole query (caller/prefetch thread, persistent segment workers), so each easy
  // handle's own connection cache still gives keep-alive reuse; shared TLS sessions keep new
  // handshakes cheap.
}

CurlShare::~CurlShare() {
  if (share_) curl_share_cleanup(static_cast<CURLSH*>(share_));
}

void CurlShare::lockCallback(void*, int data, int, void* userptr) {
  auto* self = static_cast<CurlShare*>(userptr);
  if (data >= 0 && static_cast<size_t>(data) < self->locks_.size()) self->locks_[static_cast<size_t>(data)]->lock();
}

void CurlShare::unlockCallback(void*, int data, void* userptr) {
  auto* self = static_cast<CurlShare*>(userptr);
  if (data >= 0 && static_cast<size_t>(data) < self->locks_.size()) self->locks_[static_cast<size_t>(data)]->unlock();
}

// ---- HttpResponse -----------------------------------------------------------------------------

std::string HttpResponse::header(std::string_view name) const {
  auto it = headers.find(std::string(name));
  return it == headers.end() ? std::string() : it->second;
}

std::vector<std::string> HttpResponse::headerValues(std::string_view name) const {
  std::vector<std::string> out;
  auto range = headers.equal_range(std::string(name));
  for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
  return out;
}

// ---- HttpClient -------------------------------------------------------------------------------

struct HttpClient::Impl {
  CURL* easy = nullptr;
  char errbuf[CURL_ERROR_SIZE] = {0};
  HttpResponse* current = nullptr;   // response being filled during perform()
  bool basicOrNegotiateConfigured = false;
  std::string trustedOrigin;         // cached urlOrigin() of the credential scope
  ~Impl() {
    if (easy) curl_easy_cleanup(easy);
  }
};

HttpClient::HttpClient(std::shared_ptr<const ConnectionConfig> config, std::shared_ptr<CurlShare> share,
                       std::shared_ptr<AuthContext> auth)
    : config_(std::move(config)), share_(std::move(share)), auth_(std::move(auth)), impl_(std::make_unique<Impl>()) {
  curlGlobalInit();
  impl_->easy = curl_easy_init();
  if (!impl_->easy) throw DriverError(sqlstate::kMemoryAllocation, "curl_easy_init failed");
  applyStaticOptions();
}

HttpClient::~HttpClient() = default;

void HttpClient::setCancelFlags(const std::atomic<bool>* primary, const std::atomic<bool>* secondary) {
  cancelPrimary_ = primary;
  cancelSecondary_ = secondary;
}

void HttpClient::setDeadline(std::optional<std::chrono::steady_clock::time_point> deadline) { deadline_ = deadline; }

bool HttpClient::cancelRequested() const {
  return (cancelPrimary_ && cancelPrimary_->load(std::memory_order_relaxed)) ||
         (cancelSecondary_ && cancelSecondary_->load(std::memory_order_relaxed));
}

bool HttpClient::deadlinePassed() const {
  return deadline_.has_value() && std::chrono::steady_clock::now() >= *deadline_;
}

void HttpClient::applyStaticOptions() {
  CURL* h = impl_->easy;
  const ConnectionConfig& c = *config_;
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::max(0, c.connectTimeoutSec)));
  curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(std::max(0, c.requestTimeoutSec)));
  curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
  if (c.compression) curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, &HttpClient::progressCallback);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, this);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &HttpClient::headerCallback);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, this);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &HttpClient::writeCallback);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, this);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, impl_->errbuf);
  if (share_) curl_easy_setopt(h, CURLOPT_SHARE, static_cast<CURLSH*>(share_->handle()));

  // ---- TLS
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, c.sslVerification != SslVerification::None ? 1L : 0L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, c.sslVerification == SslVerification::Full ? 2L : 0L);
  if (!c.caFile.empty()) {
    if (curl_easy_setopt(h, CURLOPT_CAINFO, c.caFile.c_str()) != CURLE_OK)
      TRINO_LOG_WARN(kLog, "libcurl ignored TrustedCerts=%s (TLS backend without CAINFO support)", c.caFile.c_str());
  }
  if (!c.caPath.empty()) {
    if (curl_easy_setopt(h, CURLOPT_CAPATH, c.caPath.c_str()) != CURLE_OK)
      TRINO_LOG_WARN(kLog, "libcurl ignored CAPath=%s (TLS backend without CAPATH support)", c.caPath.c_str());
  }
  const std::string backend = curlTlsBackend();
  long sslOptions = revocationSslOptions(c, backend);
#if defined(TRINO_ODBC_WINDOWS) && defined(CURLSSLOPT_NATIVE_CA)
  if (c.useSystemTrustStore) sslOptions |= CURLSSLOPT_NATIVE_CA;
#endif
  if (sslOptions != 0) curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, sslOptions);
  curl_easy_setopt(h, CURLOPT_SSLVERSION,
                   c.minTlsVersion == "1.3" ? static_cast<long>(CURL_SSLVERSION_TLSv1_3)
                                            : static_cast<long>(CURL_SSLVERSION_TLSv1_2));
  if (!c.clientCert.empty()) {
    checkClientCertificateSupport(c, backend);  // Schannel: PKCS#12 or store path only
    if (isSchannel(backend)) {
      if (isPkcs12Path(c.clientCert)) {
        curl_easy_setopt(h, CURLOPT_SSLCERTTYPE, "P12");
        bool viaBlob = false;
#if LIBCURL_VERSION_NUM >= 0x074700  // CURLOPT_SSLCERT_BLOB: 7.71.0
        // Handing the bytes over avoids libcurl re-opening a UTF-8 path with the ANSI file API.
        std::string bytes;
        if (readWholeFile(c.clientCert, &bytes) && !bytes.empty()) {
          curl_blob blob;
          blob.data = &bytes[0];
          blob.len = bytes.size();
          blob.flags = CURL_BLOB_COPY;
          viaBlob = curl_easy_setopt(h, CURLOPT_SSLCERT_BLOB, &blob) == CURLE_OK;
        }
#endif
        if (!viaBlob) curl_easy_setopt(h, CURLOPT_SSLCERT, c.clientCert.c_str());
      } else {
        curl_easy_setopt(h, CURLOPT_SSLCERT, c.clientCert.c_str());  // certificate-store path
      }
      // CURLOPT_SSLKEY is ignored by Schannel (the key lives in the PKCS#12 / store entry).
    } else {
      curl_easy_setopt(h, CURLOPT_SSLCERT, c.clientCert.c_str());
      if (isPkcs12Path(c.clientCert)) curl_easy_setopt(h, CURLOPT_SSLCERTTYPE, "P12");
      if (!c.clientKey.empty()) curl_easy_setopt(h, CURLOPT_SSLKEY, c.clientKey.c_str());
    }
    if (!c.clientKeyPassword.empty()) curl_easy_setopt(h, CURLOPT_KEYPASSWD, c.clientKeyPassword.c_str());
  }

  // ---- proxies
  if (!c.httpProxy.empty()) {
    const std::string proxy = c.httpProxy.find("://") == std::string::npos ? "http://" + c.httpProxy : c.httpProxy;
    curl_easy_setopt(h, CURLOPT_PROXY, proxy.c_str());
  } else if (!c.socksProxy.empty()) {
    const std::string proxy = c.socksProxy.find("://") == std::string::npos ? "socks5h://" + c.socksProxy : c.socksProxy;
    curl_easy_setopt(h, CURLOPT_PROXY, proxy.c_str());
  }
  if ((!c.httpProxy.empty() || !c.socksProxy.empty()) && !c.proxyUser.empty()) {
    const std::string userpwd = c.proxyUser + ":" + c.proxyPassword;
    curl_easy_setopt(h, CURLOPT_PROXYUSERPWD, userpwd.c_str());
  }

  // ---- Kerberos needs a GSS-API/SSPI-enabled libcurl; fail early with a clear message.
  if (c.auth == AuthType::Kerberos) {
    if (curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NEGOTIATE)) != CURLE_OK)
      throw DriverError(sqlstate::kOptionalFeatureNotImplemented,
                        "Kerberos authentication is not available: libcurl was built without GSS-API/SPNEGO support");
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
  }
}

size_t HttpClient::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* self = static_cast<HttpClient*>(userdata);
  const size_t n = size * nmemb;
  HttpResponse* resp = self->impl_->current;
  if (!resp) return n;
  try {
    resp->body.append(ptr, n);
  } catch (const std::bad_alloc&) {
    return 0;  // curl reports CURLE_WRITE_ERROR
  }
  return n;
}

size_t HttpClient::headerCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* self = static_cast<HttpClient*>(userdata);
  const size_t n = size * nmemb;
  HttpResponse* resp = self->impl_->current;
  if (!resp) return n;
  std::string_view line(ptr, n);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.empty()) return n;
  if (istartsWith(line, "HTTP/")) {
    // A new status line starts a new header block (100 Continue, auth negotiation rounds).
    resp->headers.clear();
    return n;
  }
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) return n;
  std::string name(trim(line.substr(0, colon)));
  std::string value(trim(line.substr(colon + 1)));
  if (iequals(name, "Content-Length")) {
    int64_t len = 0;
    // Reserving once avoids repeated geometric growth on multi-megabyte pages.
    if (parseInt64(value, &len) && len > 0 && len < (1LL << 31)) {
      try {
        resp->body.reserve(static_cast<size_t>(len) + kJsonPaddingBytes);
      } catch (const std::bad_alloc&) {
        return 0;
      }
    }
  }
  resp->headers.emplace(std::move(name), std::move(value));
  return n;
}

int HttpClient::progressCallback(void* clientp, int64_t, int64_t, int64_t, int64_t) {
  auto* self = static_cast<HttpClient*>(clientp);
  return (self->cancelRequested() || self->deadlinePassed()) ? 1 : 0;
}

void HttpClient::throwCurlError(int code, const std::string& url, const char* detail) {
  const std::string host = hostOf(url);
  std::string msg = curl_easy_strerror(static_cast<CURLcode>(code));
  if (detail && *detail) {
    msg += ": ";
    msg += detail;
  }
  msg += " [" + host + "]";
  const char* state = sqlstate::kCommunicationLinkFailure;
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
      state = connectPhase_ ? sqlstate::kConnectionTimeout : sqlstate::kTimeout;
      break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
      state = connectPhase_ ? sqlstate::kUnableToConnect : sqlstate::kCommunicationLinkFailure;
      break;
    default:
      break;
  }
  TRINO_LOG_ERROR(kLog, "curl error %d (%s): %s", code, state, msg.c_str());
  throw HttpError(state, msg, 0, url, code);
}

HttpResponse HttpClient::perform(const HttpRequest& req) {
  if (cancelRequested()) throw CancelledError();
  if (deadlinePassed()) throw HttpError(sqlstate::kTimeout, "Query timeout expired", 0, req.url);

  CURL* h = impl_->easy;
  const ConnectionConfig& c = *config_;
  HttpResponse resp;
  impl_->current = &resp;
  impl_->errbuf[0] = '\0';

  curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(h, CURLOPT_TIMEOUT,
                   static_cast<long>(req.timeoutSec >= 0 ? req.timeoutSec : std::max(0, c.requestTimeoutSec)));

  // ---- method
  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, nullptr);
  switch (req.method) {
    case HttpRequest::Method::Get:
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      break;
    case HttpRequest::Method::Post:
      curl_easy_setopt(h, CURLOPT_POST, 1L);
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.data());
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
      break;
    case HttpRequest::Method::Delete:
      curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
      curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
  }

  // ---- credentials
  // Credentials belong to the configured coordinator only. A nextUri (or cancel URI) that names a
  // different scheme, host or port — e.g. http:// behind a TLS-terminating proxy that does not
  // forward the original scheme — would otherwise receive the Basic password or bearer token.
  const bool attachesCredentials =
      req.sendAuth && (c.auth == AuthType::Ldap || c.auth == AuthType::Kerberos || (auth_ && !auth_->bearer().empty()));
  if (attachesCredentials) {
    if (impl_->trustedOrigin.empty()) {
      if (auth_) impl_->trustedOrigin = auth_->origin();
      if (impl_->trustedOrigin.empty()) impl_->trustedOrigin = urlOrigin(c.baseUrl());
    }
    const std::string target = urlOrigin(req.url);
    if (target != impl_->trustedOrigin) {
      TRINO_LOG_ERROR(kLog, "refusing to send credentials to %s (configured server is %s)",
                      target.empty() ? "<unparsable URL>" : target.c_str(), impl_->trustedOrigin.c_str());
      throw HttpError(sqlstate::kCommunicationLinkFailure,
                      "The server redirected the query to " + (target.empty() ? std::string("an invalid URL") : target) +
                          ", which differs from the configured server " + impl_->trustedOrigin +
                          "; the request was refused so that credentials are not sent there (check the "
                          "coordinator's http-server.process-forwarded / proxy configuration)",
                      0, redactUrlForLog(req.url, false));
    }
  }
  SList headers;
  if (req.sendAuth) {
    switch (c.auth) {
      case AuthType::Ldap:
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(h, CURLOPT_USERNAME, c.user.c_str());
        curl_easy_setopt(h, CURLOPT_PASSWORD, c.password.c_str());
        break;
      case AuthType::Kerberos:
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NEGOTIATE));
        curl_easy_setopt(h, CURLOPT_USERPWD, ":");
        curl_easy_setopt(h, CURLOPT_SERVICE_NAME, c.kerberosServiceName.empty() ? "HTTP" : c.kerberosServiceName.c_str());
        curl_easy_setopt(h, CURLOPT_GSSAPI_DELEGATION,
                         static_cast<long>(c.kerberosDelegation ? CURLGSSAPI_DELEGATION_FLAG : CURLGSSAPI_DELEGATION_NONE));
        break;
      case AuthType::Jwt:
      case AuthType::OAuth2:
      case AuthType::Certificate:
      case AuthType::None:
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
        curl_easy_setopt(h, CURLOPT_USERNAME, nullptr);
        curl_easy_setopt(h, CURLOPT_PASSWORD, nullptr);
        break;
    }
    if (auth_) {
      std::string token = auth_->bearer();
      if (!token.empty()) {
        headers.add("Authorization: Bearer " + token);
        resp.sentBearer = std::move(token);
      }
    }
  } else {
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    curl_easy_setopt(h, CURLOPT_USERNAME, nullptr);
    curl_easy_setopt(h, CURLOPT_PASSWORD, nullptr);
  }

  // ---- headers
  // Header names/values come partly from DSN and connection-string text; a CR/LF (or any other
  // control character) would let a caller smuggle extra headers into the request.
  const auto addHeader = [&headers](const std::string& name, const std::string& value) {
    const auto bad = [](unsigned char ch) { return ch < 0x20 || ch == 0x7F; };
    if (name.empty() || std::any_of(name.begin(), name.end(), bad) || name.find(':') != std::string::npos ||
        std::any_of(value.begin(), value.end(), bad))
      throw DriverError(sqlstate::kGeneralError, "Invalid character in HTTP header '" + name + "'");
    if (value.empty()) headers.add(name + ";");  // curl syntax for an empty-valued header
    else headers.add(name + ": " + value);
  };
  if (req.method == HttpRequest::Method::Post) headers.add("Expect:");  // no 100-continue round trip
  for (const auto& kv : req.headers) addHeader(kv.first, kv.second);
  if (req.sendExtraHeaders) {
    for (const auto& kv : c.extraHeaders) addHeader(kv.first, kv.second);
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.list);

  // URLs are logged redacted: presigned segment URIs and the OAuth2 token-poll URL are capabilities.
  if (TRINO_LOG_ENABLED(LogLevel::Debug))
    TRINO_LOG_DEBUG(kLog, "%s %s (%zu body bytes)", methodName(req.method), redactUrlForLog(req.url, !req.sendAuth).c_str(),
                    req.body.size());

  const auto started = std::chrono::steady_clock::now();
  const CURLcode rc = curl_easy_perform(h);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);
  impl_->current = nullptr;

  if (rc != CURLE_OK) {
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
      if (cancelRequested()) throw CancelledError();
      if (deadlinePassed()) {
        TRINO_LOG_WARN(kLog, "%s %s aborted: query timeout expired", methodName(req.method),
                       redactUrlForLog(req.url, !req.sendAuth).c_str());
        throw HttpError(sqlstate::kTimeout, "Query timeout expired", 0, req.url, rc);
      }
      throw CancelledError();
    }
    if (rc == CURLE_WRITE_ERROR)
      throw DriverError(sqlstate::kMemoryAllocation, "Out of memory while receiving the response");
    throwCurlError(rc, req.url, impl_->errbuf);
  }

  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  resp.status = status;
  char* effective = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective) resp.effectiveUrl = effective;
  ensureJsonPadding(resp.body);

  if (TRINO_LOG_ENABLED(LogLevel::Debug)) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    TRINO_LOG_DEBUG(kLog, "%s %s -> %ld (%zu bytes, %lld ms)", methodName(req.method),
                    redactUrlForLog(req.url, !req.sendAuth).c_str(), status, resp.body.size(), static_cast<long long>(ms));
  }
  return resp;
}

bool HttpClient::isRetriableCurlError(int code, bool connectLevelOnly) {
  switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_SSL_CONNECT_ERROR:
      return true;
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
      return !connectLevelOnly;
    default:
      return false;
  }
}

bool HttpClient::interruptibleSleep(int ms) const {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (true) {
    if (cancelRequested() || deadlinePassed()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) return true;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(remaining, kSleepSliceMs)));
  }
}

HttpResponse HttpClient::performWithRetry(const HttpRequest& req, int maxRetries) {
  if (maxRetries < 0) maxRetries = 0;
  for (int attempt = 0;; ++attempt) {
    bool retry = false;
    std::string reason;
    try {
      HttpResponse resp = perform(req);
      if (!isRetriableStatus(resp.status) || attempt >= maxRetries) return resp;
      reason = "HTTP " + std::to_string(resp.status);
      retry = true;
    } catch (const HttpError& e) {
      if (attempt >= maxRetries || !isRetriableCurlError(e.curlCode(), !req.idempotent)) throw;
      reason = e.message();
      retry = true;
    }
    if (!retry) break;
    // 100 ms * 2^attempt capped at 2 s, plus a deterministic jitter so that several statements
    // retrying at once do not hit the coordinator in lock step.
    const int base = attempt >= 5 ? 2000 : std::min(2000, 100 << attempt);
    const int jitter = static_cast<int>((++jitterCounter_ * 37u) % 100u);
    const int ms = base + jitter;
    TRINO_LOG_WARN(kLog, "%s %s failed (%s); retry %d/%d in %d ms", methodName(req.method),
                   redactUrlForLog(req.url, !req.sendAuth).c_str(), reason.c_str(), attempt + 1, maxRetries, ms);
    if (!interruptibleSleep(ms)) {
      if (cancelRequested()) throw CancelledError();
      throw HttpError(sqlstate::kTimeout, "Query timeout expired while waiting to retry", 0, req.url);
    }
  }
  throw HttpError(sqlstate::kCommunicationLinkFailure, "Request failed", 0, req.url);  // unreachable
}

void HttpClient::requireSuccess(const HttpResponse& resp, const std::string& url) {
  if (resp.ok()) return;
  if (resp.status == 401) {
    std::string msg = "Authentication failed (HTTP 401)";
    const std::string challenge = resp.header("WWW-Authenticate");
    if (!challenge.empty()) msg += ": server requires " + challenge;
    throw HttpError(sqlstate::kInvalidAuth, msg, resp.status, url);
  }
  if (resp.status == 403) throw HttpError(sqlstate::kInvalidAuth, "Access denied (HTTP 403)", resp.status, url);
  throw HttpError(sqlstate::kCommunicationLinkFailure,
                  "HTTP " + std::to_string(resp.status) + ": " + bodyExcerpt(resp.body), resp.status, url);
}

std::string HttpClient::escapeComponent(std::string_view s) { return urlEncode(s); }

std::string HttpClient::bodyExcerpt(std::string_view body) {
  std::string out;
  const size_t limit = std::min<size_t>(body.size(), 200);
  out.reserve(limit + 3);
  for (size_t i = 0; i < limit; ++i) {
    const unsigned char ch = static_cast<unsigned char>(body[i]);
    out.push_back(ch < 0x20 ? ' ' : static_cast<char>(ch));
  }
  if (body.size() > limit) out += "...";
  return out;
}

}  // namespace trino_odbc
