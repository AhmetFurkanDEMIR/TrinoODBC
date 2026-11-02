#pragma once
// OAuth2 "external authentication" (browser redirect) flow as implemented by Trino's
// OAuth2 authenticator and the JDBC/CLI clients. See CLIENT-DETAILS.md.
#include "client/http_client.h"
#include "core/config.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

namespace trino_odbc {

/// Parses `Bearer x_redirect_server="<url>", x_token_server="<url>"` (parameters in any order,
/// quoted or bare, other challenges/parameters ignored). Returns false unless both URLs are found.
bool parseBearerChallenge(std::string_view header, std::string* redirect, std::string* tokenServer);

class OAuth2Authenticator {
 public:
  /// `cacheKey` identifies the token cache entry (baseUrl + user). `cancel` (nullable) aborts the
  /// polling loop with CancelledError.
  OAuth2Authenticator(std::string cacheKey, int timeoutSec, RedirectHandler handler, const std::atomic<bool>* cancel);

  /// Runs the browser flow for `challenge` (the WWW-Authenticate value) and returns the token,
  /// caching it process-wide. Throws DriverError(28000) on denial/timeout, HttpError on transport
  /// failure, CancelledError when cancelled.
  std::string obtainToken(HttpClient& http, const std::string& challenge);
  /// Token cached for this key, if any.
  std::optional<std::string> cachedToken() const;
  /// Forgets the cached token, but only when it still equals `token` (another connection may have
  /// refreshed it in the meantime).
  void dropCachedToken(const std::string& token);
  /// When false the token is kept out of the process-wide cache (cachedToken() then never finds
  /// one). Used for connections without UID, whose cache key is private to the connection: storing
  /// their tokens would only accumulate live credentials nobody can ever reuse.
  void setUseProcessCache(bool on) { useProcessCache_ = on; }

  /// Returns the token to retry with after the server rejected `rejected` (the token the failed
  /// request actually carried, "" when it carried none):
  ///  * a different token already published in `auth` or in the process cache (another statement
  ///    or connection finished a login meanwhile) is returned without a new browser flow;
  ///  * otherwise the rejected token is dropped from the cache and one browser flow runs. Flows are
  ///    serialised per cache key: concurrent 401s wait for that single login and reuse its token.
  /// Throws like obtainToken(); waiting is interruptible by the cancel flag.
  std::string refreshToken(HttpClient& http, const std::string& challenge, const std::string& rejected,
                           AuthContext* auth);

  /// Opens `url` in the user's browser. False when no launcher is available.
  static bool launchBrowser(const std::string& url);

 private:
  std::string cacheKey_;
  int timeoutSec_;
  RedirectHandler handler_;
  const std::atomic<bool>* cancel_;
  bool useProcessCache_ = true;
};

}  // namespace trino_odbc
