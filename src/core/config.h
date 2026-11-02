#pragma once
// Connection configuration: every DSN / connection-string key, alias resolution, defaults and
// validation. See ARCHITECTURE.md §6 for the key table (that table is the spec for this file).
#include "platform.h"
#include "util/strings.h"
#include "util/unicode.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

enum class AuthType { None = 0, Ldap = 1, Kerberos = 2, Jwt = 3, OAuth2 = 4, Certificate = 5 };
enum class SslVerification { Full, Ca, None };
/// How `timestamp with time zone` values are delivered. Session (the default) converts the instant to
/// the session time zone and exposes the wall clock there; Utc converts to UTC; WallClock keeps the
/// digits as sent (zone dropped); String keeps Trino's text (character column).
enum class TimestampTzMode { Utc, WallClock, String, Session };
enum class TimeTzMode { Utc, WallClock, String, Session };
enum class IntervalMode { String, Interval };
enum class RedirectHandler { Open, Print };

/// Project default session time zone (Turkey time, UTC+03:00 all year since 2016). Used when the
/// TimeZone key is not set: sent as X-Trino-Time-Zone and used by TimestampWithTimeZoneMode=session.
/// This is a deliberate project default; TimeZone=system selects the operating-system zone.
inline constexpr const char* kDefaultSessionTimeZone = "Europe/Istanbul";
/// TimeZone keyword that selects the operating-system time zone.
inline constexpr const char* kSystemTimeZoneKeyword = "system";

/// The IANA name (or offset) the driver really uses for a configured TimeZone value:
/// "" -> kDefaultSessionTimeZone, "system" -> the OS zone ("" when it cannot be determined, in which
/// case no X-Trino-Time-Zone header is sent), anything else unchanged (trimmed).
std::string resolveSessionTimeZone(std::string_view configured);

const char* authTypeName(AuthType t);
const char* sslVerificationName(SslVerification v);
bool parseAuthType(std::string_view s, AuthType* out);
bool parseSslVerification(std::string_view s, SslVerification* out);

/// Options that drive Trino-type -> ODBC-type mapping (derived from the config).
struct TypeMappingOptions {
  bool unicode = true;
  int defaultStringLength = 65535;
  bool unboundedAsLong = false;
  TimestampTzMode timestampTz = TimestampTzMode::Session;
  TimeTzMode timeTz = TimeTzMode::String;
  IntervalMode interval = IntervalMode::String;
  bool uuidAsGuid = false;
  bool decimalAsDouble = false;
};

struct ConnectionConfig {
  // --- identity
  std::string dsn, driver, description;
  // --- endpoint
  std::string host;
  int port = 0;                      // 0 = not set (finalize() picks 8080 / 443)
  std::string catalog, schema;
  // --- auth
  std::string user, password;
  AuthType auth = AuthType::None;
  bool authExplicit = false;         // AuthenticationType given explicitly
  std::string accessToken, accessTokenFile;
  std::string kerberosServiceName = "HTTP";
  bool kerberosDelegation = false;
  bool kerberosUseCanonicalHostname = true;
  int externalAuthTimeoutSec = 120;
  RedirectHandler externalAuthRedirectHandler = RedirectHandler::Open;
  bool allowInsecurePassword = false;
  // --- TLS
  bool ssl = false;
  bool sslExplicit = false;
  SslVerification sslVerification = SslVerification::Full;
  std::string caFile, caPath, clientCert, clientKey, clientKeyPassword;
  bool useSystemTrustStore = true;
  std::string minTlsVersion = "1.2";
  // --- session
  std::string source = "trino-odbc";
  std::string clientInfo, traceToken, locale, sessionUser;
  /// TimeZone key: IANA name / offset, "system" (OS zone), or "" before finalize(); finalize() fills
  /// kDefaultSessionTimeZone when empty. Use resolveSessionTimeZone() for the effective zone.
  std::string timeZone;
  std::vector<std::string> clientTags;
  std::map<std::string, std::string> sessionProperties;
  std::map<std::string, std::string> extraCredentials;
  std::map<std::string, std::string> roles;        // catalog -> "ROLE{x}" / "ALL" / "NONE"
  std::vector<std::pair<std::string, std::string>> extraHeaders;
  // --- network
  std::string httpProxy, socksProxy, proxyUser, proxyPassword;
  int connectTimeoutSec = 30;
  int requestTimeoutSec = 60;
  int queryTimeoutSec = 0;
  /// IdleFetchTimeout: seconds without any application call on a statement whose server query is
  /// still running after which the driver cancels that query (0 = off).
  int idleFetchTimeoutSec = 0;
  int maxRetries = 5;
  bool compression = true;
  // --- result transfer
  bool spooling = true;
  std::string queryDataEncoding = "json+zstd,json+lz4,json";
  int targetResultSizeMb = 16;
  int prefetchPages = 4;
  int64_t maxPrefetchBytes = 64LL * 1024 * 1024;
  int maxParallelSegments = 4;
  // --- type mapping
  bool unicodeTypes = true;
  int defaultStringLength = 65535;
  bool unboundedVarcharAsLong = false;
  TimestampTzMode timestampTzMode = TimestampTzMode::Session;
  TimeTzMode timeTzMode = TimeTzMode::String;
  IntervalMode intervalMode = IntervalMode::String;
  bool uuidAsGuid = false;
  bool complexTypesAsJson = true;
  bool decimalAsDouble = false;
  // --- behaviour
  bool explicitPrepare = false;
  bool metadataCaseSensitive = false;
  bool enableTransactions = false;
  bool validateConnection = true;
  bool readOnly = false;
  AnsiEncoding ansiEncoding = defaultAnsiEncoding();
  int logLevel = 0;
  std::string logPath;
  /// Keys we did not recognise (kept for diagnostics / SQLBrowseConnect echo).
  KeyValueMap unknown;

  // --- parsing -------------------------------------------------------------------------------
  /// Parses `Key=Value;Key2={va;lue}` (ODBC connection-string grammar: braces, quotes, escaped braces).
  static KeyValueMap parseConnectionString(std::string_view connStr);
  /// Applies one key (alias-aware, case-insensitive). Unknown keys go to `unknown`.
  /// Returns false when the value is malformed (message in *error).
  bool applyKeyValue(std::string_view key, std::string_view value, std::string* error);
  /// Applies every key of a parsed map. Throws DriverError(HY000 / 01S00) on malformed values.
  void applyAll(const KeyValueMap& kv);
  /// Loads every key from the DSN section via SQLGetPrivateProfileString. Keys already set from the
  /// connection string are NOT overwritten when `onlyMissing` is true.
  void loadDsn(std::string_view dsnName, bool onlyMissing = true);
  /// Applies derived defaults (port, ssl, auth, TimeZone) — call after all keys are applied.
  void finalize();
  /// Throws DriverError with the right SQLSTATE (28000 / HY000 / HY024) when inconsistent.
  void validate() const;
  /// Canonical connection string (password masked unless includeSecrets).
  std::string toConnectionString(bool includeSecrets) const;
  /// "https://host:port"
  std::string baseUrl() const;
  TypeMappingOptions typeMapping() const;
  /// True when a Trino user name is required by this auth mode but not set.
  bool needsUser() const;
  /// Canonical key names in documentation order (used by ConfigDSN and SQLBrowseConnect).
  static const std::vector<std::string>& knownKeys();
  /// Resolves an alias to its canonical key ("" when unknown).
  static std::string canonicalKey(std::string_view key);
  /// Names of keys whose values are secrets (never logged).
  static bool isSecretKey(std::string_view canonicalKey);

 private:
  std::vector<std::string> setKeys_;  // canonical keys that were explicitly set
};

}  // namespace trino_odbc
