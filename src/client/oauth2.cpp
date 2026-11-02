// OAuth2 external (browser) authentication. See oauth2.h.
#include "client/oauth2.h"

#include "result/json_decoder.h"
#include "util/log.h"
#include "util/strings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#if defined(TRINO_ODBC_WINDOWS)
#  include <shellapi.h>
#elif defined(TRINO_ODBC_UNIX)
#  include <spawn.h>
#  include <sys/types.h>
#  include <sys/wait.h>
extern char** environ;
#endif

namespace trino_odbc {

namespace {
constexpr const char* kLog = "oauth2";
constexpr int kPollIntervalMs = 1000;
constexpr int kSleepSliceMs = 50;
constexpr int kDefaultTimeoutSec = 120;

std::mutex& cacheMutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::string>& tokenCache() {
  static std::map<std::string, std::string> cache;
  return cache;
}
/// Cache keys with a browser flow in progress (under cacheMutex()); flowDone() is notified when
/// one ends so that waiting statements re-check the cache.
std::set<std::string>& flowsInProgress() {
  static std::set<std::string> keys;
  return keys;
}
std::condition_variable& flowDone() {
  static std::condition_variable cv;
  return cv;
}

bool isTokenChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; }

/// Finds `name=value` (value quoted or bare) anywhere in an auth-param list.
bool extractParam(std::string_view header, const char* name, std::string* out) {
  const std::string lower = toLower(header);
  const size_t nameLen = std::strlen(name);
  size_t pos = 0;
  while ((pos = lower.find(name, pos)) != std::string::npos) {
    const bool boundaryBefore = pos == 0 || !isTokenChar(lower[pos - 1]);
    size_t p = pos + nameLen;
    if (!boundaryBefore || (p < header.size() && isTokenChar(header[p]))) {
      pos = p;
      continue;
    }
    while (p < header.size() && (header[p] == ' ' || header[p] == '\t')) ++p;
    if (p >= header.size() || header[p] != '=') {
      pos = p;
      continue;
    }
    ++p;
    while (p < header.size() && (header[p] == ' ' || header[p] == '\t')) ++p;
    std::string value;
    if (p < header.size() && header[p] == '"') {
      ++p;
      while (p < header.size() && header[p] != '"') {
        if (header[p] == '\\' && p + 1 < header.size()) ++p;
        value.push_back(header[p]);
        ++p;
      }
    } else {
      const size_t end = header.find_first_of(", \t\r\n", p);
      value = std::string(header.substr(p, end == std::string_view::npos ? std::string_view::npos : end - p));
    }
    if (value.empty()) return false;
    *out = std::move(value);
    return true;
  }
  return false;
}

bool cancelled(const std::atomic<bool>* flag) { return flag && flag->load(std::memory_order_relaxed); }
}  // namespace

bool parseBearerChallenge(std::string_view header, std::string* redirect, std::string* tokenServer) {
  // The challenge must be a Bearer one; Trino may list other schemes (Basic ...) on the same line.
  const std::string lower = toLower(header);
  size_t bearer = 0;
  bool found = false;
  while ((bearer = lower.find("bearer", bearer)) != std::string::npos) {
    const bool before = bearer == 0 || !isTokenChar(lower[bearer - 1]);
    const size_t after = bearer + 6;
    const bool afterOk = after >= lower.size() || !isTokenChar(lower[after]);
    if (before && afterOk) {
      found = true;
      break;
    }
    bearer = after;
  }
  if (!found) return false;
  std::string r, t;
  if (!extractParam(header, "x_redirect_server", &r) || !extractParam(header, "x_token_server", &t)) return false;
  if (redirect) *redirect = std::move(r);
  if (tokenServer) *tokenServer = std::move(t);
  return true;
}

OAuth2Authenticator::OAuth2Authenticator(std::string cacheKey, int timeoutSec, RedirectHandler handler,
                                         const std::atomic<bool>* cancel)
    : cacheKey_(std::move(cacheKey)),
      timeoutSec_(timeoutSec > 0 ? timeoutSec : kDefaultTimeoutSec),
      handler_(handler),
      cancel_(cancel) {}

std::optional<std::string> OAuth2Authenticator::cachedToken() const {
  if (!useProcessCache_) return std::nullopt;
  std::lock_guard<std::mutex> g(cacheMutex());
  auto it = tokenCache().find(cacheKey_);
  if (it == tokenCache().end()) return std::nullopt;
  return it->second;
}

void OAuth2Authenticator::dropCachedToken(const std::string& token) {
  std::lock_guard<std::mutex> g(cacheMutex());
  auto it = tokenCache().find(cacheKey_);
  if (it != tokenCache().end() && it->second == token) tokenCache().erase(it);
}

std::string OAuth2Authenticator::refreshToken(HttpClient& http, const std::string& challenge,
                                              const std::string& rejected, AuthContext* auth) {
  // A token different from the rejected one, published by whoever finished a login first.
  const auto replacement = [&]() -> std::string {
    if (auth) {
      std::string current = auth->bearer();
      if (!current.empty() && current != rejected) return current;
    }
    if (useProcessCache_) {
      auto it = tokenCache().find(cacheKey_);
      if (it != tokenCache().end() && !it->second.empty() && it->second != rejected) return it->second;
    }
    return std::string();
  };
  {
    std::unique_lock<std::mutex> lk(cacheMutex());
    for (;;) {
      std::string other = replacement();
      if (!other.empty()) {
        TRINO_LOG_DEBUG(kLog, "retrying with an OAuth2 token obtained by another request");
        return other;
      }
      if (flowsInProgress().count(cacheKey_) == 0) break;
      if (cancelled(cancel_)) throw CancelledError("Connection cancelled during external authentication");
      flowDone().wait_for(lk, std::chrono::milliseconds(kSleepSliceMs));
    }
    if (!rejected.empty()) {
      auto it = tokenCache().find(cacheKey_);
      if (it != tokenCache().end() && it->second == rejected) tokenCache().erase(it);
    }
    flowsInProgress().insert(cacheKey_);
  }
  struct FlowGuard {
    const std::string& key;
    ~FlowGuard() {
      {
        std::lock_guard<std::mutex> g(cacheMutex());
        flowsInProgress().erase(key);
      }
      flowDone().notify_all();
    }
  } guard{cacheKey_};
  std::string token = obtainToken(http, challenge);
  // Publish before the guard wakes the waiters, so they find it instead of starting a flow.
  if (auth) auth->setBearer(token);
  return token;
}

bool OAuth2Authenticator::launchBrowser(const std::string& url) {
  // Only http(s) URLs are ever handed to the OS launcher: a hostile challenge must not be able to
  // run arbitrary URL schemes.
  if (!istartsWith(url, "https://") && !istartsWith(url, "http://")) {
    TRINO_LOG_WARN(kLog, "Refusing to open non-http redirect URL");
    return false;
  }
#if defined(TRINO_ODBC_WINDOWS)
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
  if (wlen <= 0) return false;
  std::wstring wide(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wide[0], wlen);
  const HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(rc) > 32;
#elif defined(TRINO_ODBC_UNIX)
#  if defined(TRINO_ODBC_MACOS)
  const char* launcher = "open";
#  else
  const char* launcher = "xdg-open";
#  endif
  std::string urlCopy = url;
  char* argv[] = {const_cast<char*>(launcher), urlCopy.data(), nullptr};
  pid_t pid = 0;
  // posix_spawnp: no shell, so the URL is never interpreted.
  const int rc = posix_spawnp(&pid, launcher, nullptr, nullptr, argv, environ);
  if (rc != 0) {
    TRINO_LOG_WARN(kLog, "%s failed to start (errno %d)", launcher, rc);
    return false;
  }
  // Reap the launcher in the background: some launchers linger until the browser exits and the
  // connect call must not wait for them. startDetachedHelper pins the driver image so the reaper
  // cannot outlive it after the Driver Manager unloads the driver.
  if (!startDetachedHelper([pid] {
        int status = 0;
        (void)waitpid(pid, &status, 0);
      })) {
    TRINO_LOG_DEBUG(kLog, "cannot start the launcher reaper; pid %d stays a zombie until the process exits",
                    static_cast<int>(pid));
  }
  return true;
#else
  (void)url;
  return false;
#endif
}

std::string OAuth2Authenticator::obtainToken(HttpClient& http, const std::string& challenge) {
  std::string redirect, tokenServer;
  if (!parseBearerChallenge(challenge, &redirect, &tokenServer))
    throw DriverError(sqlstate::kInvalidAuth,
                      "The server requires OAuth2 external authentication but sent an unexpected challenge: " + challenge);

  TRINO_LOG_INFO(kLog, "External authentication required. Open this URL in a browser: %s", redirect.c_str());
  if (handler_ == RedirectHandler::Open) {
    if (!launchBrowser(redirect))
      TRINO_LOG_WARN(kLog, "Could not launch a browser automatically; open the URL manually: %s", redirect.c_str());
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec_);
  std::string uri = tokenServer;
  for (;;) {
    if (cancelled(cancel_)) throw CancelledError("Connection cancelled during external authentication");
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      throw DriverError(sqlstate::kInvalidAuth,
                        formatString("External authentication timed out after %d seconds", timeoutSec_));
    const long long remainingSec =
        std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count() + 1;

    HttpRequest req;
    req.method = HttpRequest::Method::Get;
    req.url = uri;
    req.sendAuth = false;   // the token endpoint must be polled anonymously
    req.timeoutSec = static_cast<int>(std::max<long long>(5, std::min<long long>(remainingSec, 30)));
    const auto started = std::chrono::steady_clock::now();
    HttpResponse resp = http.performWithRetry(req, 2);

    if (resp.status == 200) {
      std::string token, nextUri, error;
      if (!parseOAuthTokenResponse(resp.body, &token, &nextUri, &error))
        throw DriverError(sqlstate::kInvalidAuth, "Unexpected answer from the OAuth2 token server");
      if (!token.empty()) {
        if (useProcessCache_) {
          std::lock_guard<std::mutex> g(cacheMutex());
          tokenCache()[cacheKey_] = token;
        }
        TRINO_LOG_INFO(kLog, "External authentication succeeded");
        return token;
      }
      if (!error.empty()) throw DriverError(sqlstate::kInvalidAuth, "External authentication failed: " + error);
      if (!nextUri.empty()) uri = nextUri;
    } else if (resp.status >= 400 && resp.status < 500) {
      throw DriverError(sqlstate::kInvalidAuth,
                        formatString("External authentication was rejected (HTTP %ld): %s", resp.status,
                                     HttpClient::bodyExcerpt(resp.body).c_str()));
    } else if (!resp.ok()) {
      HttpClient::requireSuccess(resp, uri);
    }

    // Pace the polling to ~1 s unless the server already held the request that long.
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    long long waitMs = kPollIntervalMs - elapsedMs;
    while (waitMs > 0) {
      if (cancelled(cancel_)) throw CancelledError("Connection cancelled during external authentication");
      const long long slice = std::min<long long>(waitMs, kSleepSliceMs);
      std::this_thread::sleep_for(std::chrono::milliseconds(slice));
      waitMs -= slice;
    }
  }
}

}  // namespace trino_odbc
