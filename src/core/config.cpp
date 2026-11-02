// Connection configuration: connection-string grammar, key/alias table, DSN loading, derived
// defaults and validation. ARCHITECTURE.md §6 is the specification for every key handled here.
#include "core/config.h"

#include "util/errors.h"
#include "util/log.h"
#include "util/tz.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace trino_odbc {

namespace {

constexpr const char* kLogComponent = "config";
constexpr const char* kOdbcIniFile = "odbc.ini";
constexpr int64_t kMegabyte = 1024LL * 1024;
constexpr int kMaxTargetResultSizeMb = 128;

enum class Key {
  Dsn, Driver, Description,
  Host, Port, Catalog, Schema,
  Uid, Pwd, AuthenticationType,
  Ssl, SslVerification, AllowSelfSignedServerCert, AllowHostNameCNMismatch,
  TrustedCerts, CaPath, UseSystemTrustStore, ClientCert, ClientKey, ClientKeyPassword, MinTlsVersion,
  AllowInsecurePassword, AccessToken, AccessTokenFile,
  KerberosServiceName, KerberosDelegation, KerberosUseCanonicalHostname,
  ExternalAuthTimeout, ExternalAuthRedirectHandler,
  Source, ClientInfo, ClientTags, TraceToken, TimeZone, Locale,
  SessionProperties, ExtraCredentials, Roles, SessionUser,
  HttpProxy, SocksProxy, ProxyUser, ProxyPassword,
  ConnectTimeout, RequestTimeout, QueryTimeout, IdleFetchTimeout, MaxRetries, Compression,
  Spooling, QueryDataEncoding, TargetResultSize, PrefetchPages, MaxPrefetchBytes, MaxParallelSegments,
  UseUnicodeSqlCharacterTypes, DefaultStringColumnLength, UnboundedVarcharAsLongVarchar,
  TimestampWithTimeZoneMode, TimeWithTimeZoneMode, IntervalMode, UuidAsGuid, ComplexTypesAsJson, DecimalAsDouble,
  ExplicitPrepare, MetadataCaseSensitive, EnableTransactions, ValidateConnection,
  LogLevel, LogPath, ExtraHeaders, AnsiEncodingKey, ReadOnly,
};

struct KeySpec {
  Key id;
  const char* canonical;
  const char* aliases[4];  // nullptr-terminated
  bool secret;
};

// Documentation order of ARCHITECTURE.md §6. The order is observable through knownKeys() and
// toConnectionString(), so keep it stable.
constexpr KeySpec kKeys[] = {
    {Key::Dsn, "DSN", {nullptr}, false},
    {Key::Driver, "Driver", {nullptr}, false},
    {Key::Description, "Description", {nullptr}, false},
    {Key::Host, "Host", {"Server", "HostName", nullptr}, false},
    {Key::Port, "Port", {nullptr}, false},
    {Key::Catalog, "Catalog", {"Database", nullptr}, false},
    {Key::Schema, "Schema", {nullptr}, false},
    {Key::Uid, "UID", {"User", "Username", nullptr}, false},
    {Key::Pwd, "PWD", {"Password", nullptr}, true},
    {Key::AuthenticationType, "AuthenticationType", {"AuthMech", "Auth", nullptr}, false},
    {Key::Ssl, "SSL", {"UseSSL", "Encrypt", "Secure", nullptr}, false},
    {Key::SslVerification, "SSLVerification", {"SSLMode", nullptr}, false},
    {Key::AllowSelfSignedServerCert, "AllowSelfSignedServerCert", {nullptr}, false},
    {Key::AllowHostNameCNMismatch, "AllowHostNameCNMismatch", {nullptr}, false},
    {Key::TrustedCerts, "TrustedCerts", {"SSLCA", "CAFile", "SSLTrustStorePath", nullptr}, false},
    {Key::CaPath, "CAPath", {"SSLCAPath", nullptr}, false},
    {Key::UseSystemTrustStore, "UseSystemTrustStore", {nullptr}, false},
    {Key::ClientCert, "ClientCert", {"SSLCert", "SSLKeyStorePath", nullptr}, false},
    {Key::ClientKey, "ClientKey", {"SSLKey", nullptr}, false},
    {Key::ClientKeyPassword, "ClientKeyPassword", {"SSLKeyPassword", "SSLKeyStorePassword", nullptr}, true},
    {Key::MinTlsVersion, "MinTLSVersion", {nullptr}, false},
    {Key::AllowInsecurePassword, "AllowInsecurePassword", {nullptr}, false},
    {Key::AccessToken, "AccessToken", {"JWT", "AuthToken", "Token", nullptr}, true},
    {Key::AccessTokenFile, "AccessTokenFile", {nullptr}, false},
    {Key::KerberosServiceName, "KerberosServiceName", {"KerberosRemoteServiceName", "KrbServiceName", nullptr}, false},
    {Key::KerberosDelegation, "KerberosDelegation", {nullptr}, false},
    {Key::KerberosUseCanonicalHostname, "KerberosUseCanonicalHostname", {nullptr}, false},
    {Key::ExternalAuthTimeout, "ExternalAuthTimeout", {"OAuth2Timeout", nullptr}, false},
    {Key::ExternalAuthRedirectHandler, "ExternalAuthRedirectHandler", {nullptr}, false},
    {Key::Source, "Source", {"ApplicationName", nullptr}, false},
    {Key::ClientInfo, "ClientInfo", {nullptr}, false},
    {Key::ClientTags, "ClientTags", {nullptr}, false},
    {Key::TraceToken, "TraceToken", {nullptr}, false},
    {Key::TimeZone, "TimeZone", {"TimeZoneID", nullptr}, false},
    {Key::Locale, "Locale", {"Language", nullptr}, false},
    {Key::SessionProperties, "SessionProperties", {nullptr}, false},
    {Key::ExtraCredentials, "ExtraCredentials", {nullptr}, true},
    {Key::Roles, "Roles", {nullptr}, false},
    {Key::SessionUser, "SessionUser", {nullptr}, false},
    {Key::HttpProxy, "HttpProxy", {"Proxy", nullptr}, false},
    {Key::SocksProxy, "SocksProxy", {nullptr}, false},
    {Key::ProxyUser, "ProxyUser", {nullptr}, false},
    {Key::ProxyPassword, "ProxyPassword", {nullptr}, true},
    {Key::ConnectTimeout, "ConnectTimeout", {"LoginTimeout", nullptr}, false},
    {Key::RequestTimeout, "RequestTimeout", {"Timeout", nullptr}, false},
    {Key::QueryTimeout, "QueryTimeout", {nullptr}, false},
    {Key::IdleFetchTimeout, "IdleFetchTimeout", {"AbandonedQueryTimeout", nullptr}, false},
    {Key::MaxRetries, "MaxRetries", {"Retries", nullptr}, false},
    {Key::Compression, "Compression", {"EnableCompression", nullptr}, false},
    {Key::Spooling, "Spooling", {"SpoolingEnabled", nullptr}, false},
    {Key::QueryDataEncoding, "QueryDataEncoding", {nullptr}, false},
    {Key::TargetResultSize, "TargetResultSize", {nullptr}, false},
    {Key::PrefetchPages, "PrefetchPages", {nullptr}, false},
    {Key::MaxPrefetchBytes, "MaxPrefetchBytes", {"PrefetchBufferMB", nullptr}, false},
    {Key::MaxParallelSegments, "MaxParallelSegments", {nullptr}, false},
    {Key::UseUnicodeSqlCharacterTypes, "UseUnicodeSqlCharacterTypes", {"UnicodeTypes", nullptr}, false},
    {Key::DefaultStringColumnLength, "DefaultStringColumnLength", {"MaxVarcharSize", "StringColumnLength", nullptr}, false},
    {Key::UnboundedVarcharAsLongVarchar, "UnboundedVarcharAsLongVarchar", {nullptr}, false},
    {Key::TimestampWithTimeZoneMode, "TimestampWithTimeZoneMode", {"TimestampTZMode", nullptr}, false},
    {Key::TimeWithTimeZoneMode, "TimeWithTimeZoneMode", {nullptr}, false},
    {Key::IntervalMode, "IntervalMode", {nullptr}, false},
    {Key::UuidAsGuid, "UuidAsGuid", {nullptr}, false},
    {Key::ComplexTypesAsJson, "ComplexTypesAsJson", {nullptr}, false},
    {Key::DecimalAsDouble, "DecimalAsDouble", {nullptr}, false},
    {Key::ExplicitPrepare, "ExplicitPrepare", {nullptr}, false},
    {Key::MetadataCaseSensitive, "MetadataCaseSensitive", {"AssumeLiteralNamesInMetadataCalls", nullptr}, false},
    {Key::EnableTransactions, "EnableTransactions", {nullptr}, false},
    {Key::ValidateConnection, "ValidateConnection", {nullptr}, false},
    {Key::LogLevel, "LogLevel", {nullptr}, false},
    {Key::LogPath, "LogPath", {nullptr}, false},
    {Key::ExtraHeaders, "ExtraHeaders", {nullptr}, false},
    {Key::AnsiEncodingKey, "AnsiEncoding", {nullptr}, false},
    {Key::ReadOnly, "ReadOnly", {nullptr}, false},
};

const KeySpec* findKey(std::string_view key) {
  const std::string_view k = trim(key);
  if (k.empty()) return nullptr;
  for (const auto& spec : kKeys) {
    if (iequals(k, spec.canonical)) return &spec;
    for (const char* const* alias = spec.aliases; *alias != nullptr; ++alias) {
      if (iequals(k, *alias)) return &spec;
    }
  }
  return nullptr;
}

std::string invalidValue(std::string_view key, std::string_view value, std::string_view expected) {
  std::string msg = "Invalid value for ";
  msg.append(key.data(), key.size());
  msg += ": '";
  msg.append(value.data(), value.size());
  msg += "' (expected ";
  msg.append(expected.data(), expected.size());
  msg += ")";
  return msg;
}

/// Error text for a malformed entry of a list-valued key whose value must never be echoed
/// (ExtraCredentials, ExtraHeaders: they carry secrets, and the message reaches diagnostics, logs and
/// application error dialogs). Only the key and the 1-based position of the bad entry are named.
std::string invalidEntry(std::string_view key, size_t entryIndex, std::string_view expected) {
  std::string msg = "Invalid value for ";
  msg.append(key.data(), key.size());
  msg += ": entry " + std::to_string(entryIndex) + " is malformed (expected ";
  msg.append(expected.data(), expected.size());
  msg += "; the value is not shown because it may contain credentials)";
  return msg;
}

/// True when the value of `key` must not appear in error messages.
bool isRedactedKey(std::string_view key) {
  const KeySpec* spec = findKey(key);
  return spec != nullptr && (spec->secret || spec->id == Key::ExtraHeaders);
}

bool parseBoolField(std::string_view key, std::string_view value, bool* out, std::string* error) {
  if (parseBool(trim(value), out)) return true;
  if (error) *error = invalidValue(key, value, "1/0, true/false, yes/no, on/off");
  return false;
}

bool parseIntField(std::string_view key, std::string_view value, int64_t lo, int64_t hi, int64_t* out,
                   std::string* error) {
  int64_t v = 0;
  if (!parseInt64(trim(value), &v)) {
    if (error) *error = invalidValue(key, value, "an integer");
    return false;
  }
  if (v < lo || v > hi) {
    if (error) *error = invalidValue(key, value, "an integer between " + std::to_string(lo) + " and " + std::to_string(hi));
    return false;
  }
  *out = v;
  return true;
}

/// Splits `k=v` lists separated by ';' or ','. Returns false (with *error) on an entry without '='.
bool parseKeyValueList(std::string_view key, std::string_view value, bool allowComma,
                       std::vector<std::pair<std::string, std::string>>* out, std::string* error) {
  out->clear();
  std::string current;
  size_t entryIndex = 0;
  const bool redact = isRedactedKey(key);
  auto fail = [&](std::string_view expected) {
    if (error) *error = redact ? invalidEntry(key, entryIndex, expected) : invalidValue(key, value, expected);
    return false;
  };
  auto flush = [&]() -> bool {
    const std::string_view entry = trim(current);
    if (entry.empty()) {
      current.clear();
      return true;
    }
    ++entryIndex;
    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) return fail("entries of the form name=value");
    const std::string_view name = trim(entry.substr(0, eq));
    if (name.empty()) return fail("non-empty names in name=value entries");
    out->emplace_back(std::string(name), std::string(trim(entry.substr(eq + 1))));
    current.clear();
    return true;
  };
  for (const char c : value) {
    if (c == ';' || (allowComma && c == ',')) {
      if (!flush()) return false;
    } else {
      current += c;
    }
  }
  return flush();
}

/// Normalises a role specification to Trino's header form: ALL, NONE or ROLE{name}.
std::string normalizeRole(std::string_view spec) {
  const std::string_view s = trim(spec);
  const std::string upper = toUpper(s);
  if (upper == "ALL" || upper == "NONE") return upper;
  if (istartsWith(s, "ROLE{") && endsWith(s, "}")) {
    return "ROLE{" + std::string(trim(s.substr(5, s.size() - 6))) + "}";
  }
  return "ROLE{" + std::string(s) + "}";
}

/// Quotes a connection-string value with braces when the ODBC grammar requires it.
std::string quoteConnValue(std::string_view v) {
  const bool needsBraces = !v.empty() && (v.find_first_of(";{}[]()=,?*!@\"'") != std::string_view::npos ||
                                          v.front() == ' ' || v.back() == ' ');
  if (!needsBraces) return std::string(v);
  std::string out = "{";
  for (const char c : v) {
    if (c == '}') out += "}}";  // a right brace inside braces is escaped by doubling
    else out += c;
  }
  out += "}";
  return out;
}

std::string joinPairs(const std::map<std::string, std::string>& m) {
  std::string out;
  for (const auto& [k, v] : m) {
    if (!out.empty()) out += ';';
    out += k;
    out += '=';
    out += v;
  }
  return out;
}

std::string boolText(bool b) { return b ? "1" : "0"; }

}  // namespace

std::string resolveSessionTimeZone(std::string_view configured) {
  const std::string_view tz = trim(configured);
  if (tz.empty()) return kDefaultSessionTimeZone;
  if (iequals(tz, kSystemTimeZoneKeyword)) return TimeZoneDb::systemZoneName();
  return std::string(tz);
}

namespace {

const char* timestampTzModeName(TimestampTzMode m) {
  switch (m) {
    case TimestampTzMode::Utc: return "utc";
    case TimestampTzMode::WallClock: return "wallclock";
    case TimestampTzMode::String: return "string";
    case TimestampTzMode::Session: return "session";
  }
  return "session";
}

const char* timeTzModeName(TimeTzMode m) {
  switch (m) {
    case TimeTzMode::Utc: return "utc";
    case TimeTzMode::WallClock: return "wallclock";
    case TimeTzMode::String: return "string";
    case TimeTzMode::Session: return "session";
  }
  return "string";
}

bool parseTzModeWord(std::string_view value, int* out) {
  const std::string v = toLower(trim(value));
  if (v == "utc") { *out = 0; return true; }
  if (v == "wallclock" || v == "wall_clock" || v == "wall-clock") { *out = 1; return true; }
  if (v == "string" || v == "text") { *out = 2; return true; }
  // "local" = local time of the session zone (it was an alias of wallclock before the session mode).
  if (v == "session" || v == "local" || v == "sessiontz" || v == "session_tz" || v == "session-tz") { *out = 3; return true; }
  return false;
}

// ---- SQLGetPrivateProfileString wrappers ------------------------------------------------------

#if defined(TRINO_ODBC_WINDOWS)
static_assert(sizeof(wchar_t) == sizeof(SQLWCHAR), "Windows SQLWCHAR must be wchar_t sized");

/// Raw call; returns the number of characters written (Windows semantics: cb-1 / cb-2 on truncation).
int profileStringRaw(const std::string& section, const char* entry, wchar_t* buf, int bufChars) {
  const WString wsection = utf8ToWide(section);
  WString wentry;
  if (entry) wentry = utf8ToWide(entry);
  const WString wfile = utf8ToWide(kOdbcIniFile);
  const WString wdefault;
  return SQLGetPrivateProfileStringW(reinterpret_cast<const wchar_t*>(wsection.c_str()),
                                     entry ? reinterpret_cast<const wchar_t*>(wentry.c_str()) : nullptr,
                                     reinterpret_cast<const wchar_t*>(wdefault.c_str()), buf, bufChars,
                                     reinterpret_cast<const wchar_t*>(wfile.c_str()));
}
using ProfileChar = wchar_t;
std::string profileToUtf8(const ProfileChar* s, size_t len) {
  return wideToUtf8(reinterpret_cast<const SQLWCHAR*>(s), static_cast<SQLLEN>(len));
}
#else
// unixODBC / iODBC map the narrow name to the W variant when UNICODE is defined; the driver always
// calls the narrow (UTF-8) installer API on Unix, so make sure the plain symbol is used.
#  ifdef SQLGetPrivateProfileString
#    undef SQLGetPrivateProfileString
#  endif
int profileStringRaw(const std::string& section, const char* entry, char* buf, int bufChars) {
  return SQLGetPrivateProfileString(section.c_str(), entry, "", buf, bufChars, kOdbcIniFile);
}
using ProfileChar = char;
std::string profileToUtf8(const ProfileChar* s, size_t len) { return std::string(s, len); }
#endif

/// Calls SQLGetPrivateProfileString with a growing buffer. With entry == nullptr the result is the
/// key list (NUL separated). Returns the raw characters (without the final terminator).
std::basic_string<ProfileChar> profileString(const std::string& section, const char* entry) {
  std::vector<ProfileChar> buf(1024);
  for (;;) {
    const int n = profileStringRaw(section, entry, buf.data(), static_cast<int>(buf.size()));
    if (n <= 0) return {};
    // Windows reports cb-1 (single value) or cb-2 (enumeration) when the buffer was too small;
    // unixODBC/iODBC report the copied length. Either way a nearly-full buffer means "retry bigger".
    if (static_cast<size_t>(n) + 2 < buf.size()) {
      return std::basic_string<ProfileChar>(buf.data(), static_cast<size_t>(n));
    }
    if (buf.size() >= (1u << 22)) {
      return std::basic_string<ProfileChar>(buf.data(), static_cast<size_t>(n));
    }
    buf.assign(buf.size() * 4, ProfileChar{});
  }
}

std::vector<std::string> enumerateDsnKeys(const std::string& section) {
  const auto raw = profileString(section, nullptr);
  std::vector<std::string> keys;
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = start;
    while (end < raw.size() && raw[end] != ProfileChar{}) ++end;
    if (end > start) keys.push_back(profileToUtf8(raw.data() + start, end - start));
    start = end + 1;
  }
  return keys;
}

std::optional<std::string> readDsnValue(const std::string& section, const std::string& key) {
  const auto raw = profileString(section, key.c_str());
  if (raw.empty()) return std::nullopt;
  return profileToUtf8(raw.data(), raw.size());
}

}  // namespace

// ---- enum helpers ------------------------------------------------------------------------------

const char* authTypeName(AuthType t) {
  switch (t) {
    case AuthType::None: return "None";
    case AuthType::Ldap: return "LDAP";
    case AuthType::Kerberos: return "Kerberos";
    case AuthType::Jwt: return "JWT";
    case AuthType::OAuth2: return "OAuth2";
    case AuthType::Certificate: return "Certificate";
  }
  return "None";
}

const char* sslVerificationName(SslVerification v) {
  switch (v) {
    case SslVerification::Full: return "Full";
    case SslVerification::Ca: return "CA";
    case SslVerification::None: return "None";
  }
  return "Full";
}

bool parseAuthType(std::string_view s, AuthType* out) {
  const std::string v = toLower(trim(s));
  if (v == "none" || v == "0" || v == "noauth" || v == "anonymous" || v == "no authentication") {
    *out = AuthType::None;
  } else if (v == "ldap" || v == "1" || v == "password" || v == "basic" || v == "usernamepassword" ||
             v == "userpassword" || v == "username/password" || v == "username and password") {
    *out = AuthType::Ldap;
  } else if (v == "kerberos" || v == "2" || v == "krb5" || v == "gssapi" || v == "spnego") {
    *out = AuthType::Kerberos;
  } else if (v == "jwt" || v == "3" || v == "token" || v == "accesstoken" || v == "bearer") {
    *out = AuthType::Jwt;
  } else if (v == "oauth2" || v == "4" || v == "oauth" || v == "externalauthentication" || v == "external" ||
             v == "browser") {
    *out = AuthType::OAuth2;
  } else if (v == "certificate" || v == "5" || v == "cert" || v == "clientcert" || v == "mtls") {
    *out = AuthType::Certificate;
  } else {
    return false;
  }
  return true;
}

bool parseSslVerification(std::string_view s, SslVerification* out) {
  const std::string v = toLower(trim(s));
  if (v == "full" || v == "verify-full" || v == "verify_full" || v == "verifyfull" || v == "on" || v == "true" ||
      v == "1" || v == "yes") {
    *out = SslVerification::Full;
  } else if (v == "ca" || v == "verify-ca" || v == "verify_ca" || v == "verifyca" || v == "chain") {
    *out = SslVerification::Ca;
  } else if (v == "none" || v == "off" || v == "false" || v == "0" || v == "no" || v == "disable" ||
             v == "disabled" || v == "insecure") {
    *out = SslVerification::None;
  } else {
    return false;
  }
  return true;
}

// ---- static key helpers -------------------------------------------------------------------------

const std::vector<std::string>& ConnectionConfig::knownKeys() {
  static const std::vector<std::string> keys = [] {
    std::vector<std::string> v;
    v.reserve(sizeof(kKeys) / sizeof(kKeys[0]));
    for (const auto& spec : kKeys) v.emplace_back(spec.canonical);
    return v;
  }();
  return keys;
}

std::string ConnectionConfig::canonicalKey(std::string_view key) {
  const KeySpec* spec = findKey(key);
  return spec ? std::string(spec->canonical) : std::string();
}

bool ConnectionConfig::isSecretKey(std::string_view canonicalKey) {
  const KeySpec* spec = findKey(canonicalKey);
  return spec != nullptr && spec->secret;
}

// ---- connection string grammar ------------------------------------------------------------------

KeyValueMap ConnectionConfig::parseConnectionString(std::string_view s) {
  KeyValueMap out;
  size_t pos = 0;
  const size_t n = s.size();
  auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

  while (pos < n) {
    // Skip separators and blanks between entries.
    while (pos < n && (s[pos] == ';' || isSpace(s[pos]))) ++pos;
    if (pos >= n) break;

    // Key: everything up to '=' (an entry without '=' is malformed and skipped up to the next ';').
    const size_t keyStart = pos;
    while (pos < n && s[pos] != '=' && s[pos] != ';') ++pos;
    if (pos >= n || s[pos] == ';') {
      continue;  // no '=': ignore this fragment
    }
    const std::string key(trim(s.substr(keyStart, pos - keyStart)));
    ++pos;  // consume '='

    // Value.
    while (pos < n && isSpace(s[pos])) ++pos;
    std::string value;
    if (pos < n && s[pos] == '{') {
      // Braced value: may contain ';', ends at a '}' that is not followed by another '}' ("}}" is a
      // literal brace). An unterminated brace runs to the end of the string.
      ++pos;
      for (;;) {
        if (pos >= n) break;
        const char c = s[pos];
        if (c == '}') {
          if (pos + 1 < n && s[pos + 1] == '}') {
            value += '}';
            pos += 2;
            continue;
          }
          ++pos;  // closing brace
          break;
        }
        value += c;
        ++pos;
      }
      // Ignore anything between the closing brace and the next separator.
      while (pos < n && s[pos] != ';') ++pos;
    } else if (pos < n && (s[pos] == '"' || s[pos] == '\'')) {
      // Quoted value: ends at the matching quote; a doubled quote is a literal quote.
      const char quote = s[pos];
      ++pos;
      for (;;) {
        if (pos >= n) break;
        const char c = s[pos];
        if (c == quote) {
          if (pos + 1 < n && s[pos + 1] == quote) {
            value += quote;
            pos += 2;
            continue;
          }
          ++pos;
          break;
        }
        value += c;
        ++pos;
      }
      while (pos < n && s[pos] != ';') ++pos;
    } else {
      const size_t valueStart = pos;
      while (pos < n && s[pos] != ';') ++pos;
      value = std::string(trim(s.substr(valueStart, pos - valueStart)));
    }
    if (pos < n && s[pos] == ';') ++pos;

    if (key.empty()) continue;
    out.emplace(key, std::move(value));  // first occurrence wins
  }
  return out;
}

// ---- applying keys --------------------------------------------------------------------------------

bool ConnectionConfig::applyKeyValue(std::string_view keyIn, std::string_view valueIn, std::string* error) {
  const std::string_view key = trim(keyIn);
  const std::string_view value = valueIn;
  const KeySpec* spec = findKey(key);
  if (!spec) {
    if (key.empty()) return true;
    unknown[std::string(key)] = std::string(value);
    TRINO_LOG_WARN(kLogComponent, "unknown connection option '%.*s' ignored", static_cast<int>(key.size()), key.data());
    return true;
  }

  auto markSet = [this](const char* canonical) {
    for (const auto& k : setKeys_) {
      if (iequals(k, canonical)) return;
    }
    setKeys_.emplace_back(canonical);
  };
  auto setString = [&](std::string& field) {
    field = std::string(trim(value));
    markSet(spec->canonical);
    return true;
  };
  auto setSecret = [&](std::string& field) {
    field = std::string(value);  // secrets are taken verbatim (passwords may end in spaces)
    markSet(spec->canonical);
    return true;
  };
  auto setBool = [&](bool& field) {
    if (trim(value).empty()) return true;  // empty entry in a DSN = not set
    bool b = false;
    if (!parseBoolField(spec->canonical, value, &b, error)) return false;
    field = b;
    markSet(spec->canonical);
    return true;
  };
  auto setInt = [&](int& field, int64_t lo, int64_t hi) {
    if (trim(value).empty()) return true;
    int64_t v = 0;
    if (!parseIntField(spec->canonical, value, lo, hi, &v, error)) return false;
    field = static_cast<int>(v);
    markSet(spec->canonical);
    return true;
  };
  auto setMap = [&](std::map<std::string, std::string>& field, bool allowComma) {
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!parseKeyValueList(spec->canonical, value, allowComma, &pairs, error)) return false;
    field.clear();
    for (auto& [k, v] : pairs) field[k] = v;
    markSet(spec->canonical);
    return true;
  };

  switch (spec->id) {
    case Key::Dsn: return setString(dsn);
    case Key::Driver: return setString(driver);
    case Key::Description: return setString(description);
    case Key::Host: return setString(host);
    case Key::Port: return setInt(port, 1, 65535);
    case Key::Catalog: return setString(catalog);
    case Key::Schema: return setString(schema);
    case Key::Uid: return setString(user);
    case Key::Pwd: return setSecret(password);
    case Key::AuthenticationType: {
      if (trim(value).empty()) return true;
      AuthType t = AuthType::None;
      if (!parseAuthType(value, &t)) {
        if (error) *error = invalidValue(spec->canonical, value, "None, LDAP, Kerberos, JWT, OAuth2 or Certificate");
        return false;
      }
      auth = t;
      authExplicit = true;
      markSet(spec->canonical);
      return true;
    }
    case Key::Ssl: {
      if (trim(value).empty()) return true;
      bool b = false;
      if (!parseBoolField(spec->canonical, value, &b, error)) return false;
      ssl = b;
      sslExplicit = true;
      markSet(spec->canonical);
      return true;
    }
    case Key::SslVerification: {
      if (trim(value).empty()) return true;
      SslVerification v = SslVerification::Full;
      if (!parseSslVerification(value, &v)) {
        if (error) *error = invalidValue(spec->canonical, value, "Full, CA or None");
        return false;
      }
      sslVerification = v;
      markSet(spec->canonical);
      return true;
    }
    case Key::AllowSelfSignedServerCert: {
      // Simba compatibility flag: trusting self-signed certificates means no chain verification.
      if (trim(value).empty()) return true;
      bool b = false;
      if (!parseBoolField(spec->canonical, value, &b, error)) return false;
      markSet(spec->canonical);
      if (b) {
        sslVerification = SslVerification::None;
        markSet("SSLVerification");
      }
      return true;
    }
    case Key::AllowHostNameCNMismatch: {
      // Simba compatibility flag: keep chain verification but drop the hostname check.
      if (trim(value).empty()) return true;
      bool b = false;
      if (!parseBoolField(spec->canonical, value, &b, error)) return false;
      markSet(spec->canonical);
      if (b && sslVerification == SslVerification::Full) {
        sslVerification = SslVerification::Ca;
        markSet("SSLVerification");
      }
      return true;
    }
    case Key::TrustedCerts: return setString(caFile);
    case Key::CaPath: return setString(caPath);
    case Key::UseSystemTrustStore: return setBool(useSystemTrustStore);
    case Key::ClientCert: return setString(clientCert);
    case Key::ClientKey: return setString(clientKey);
    case Key::ClientKeyPassword: return setSecret(clientKeyPassword);
    case Key::MinTlsVersion: {
      const std::string v = toLower(trim(value));
      if (v.empty()) return true;
      if (v == "1.2" || v == "tlsv1.2" || v == "tls1.2" || v == "tls12" || v == "12") {
        minTlsVersion = "1.2";
      } else if (v == "1.3" || v == "tlsv1.3" || v == "tls1.3" || v == "tls13" || v == "13") {
        minTlsVersion = "1.3";
      } else {
        if (error) *error = invalidValue(spec->canonical, value, "1.2 or 1.3");
        return false;
      }
      markSet(spec->canonical);
      return true;
    }
    case Key::AllowInsecurePassword: return setBool(allowInsecurePassword);
    case Key::AccessToken: {
      accessToken = std::string(trim(value));
      markSet(spec->canonical);
      return true;
    }
    case Key::AccessTokenFile: return setString(accessTokenFile);
    case Key::KerberosServiceName: {
      if (trim(value).empty()) return true;
      return setString(kerberosServiceName);
    }
    case Key::KerberosDelegation: return setBool(kerberosDelegation);
    case Key::KerberosUseCanonicalHostname: return setBool(kerberosUseCanonicalHostname);
    case Key::ExternalAuthTimeout: return setInt(externalAuthTimeoutSec, 1, 86400);
    case Key::ExternalAuthRedirectHandler: {
      const std::string v = toLower(trim(value));
      if (v.empty()) return true;
      if (v == "open" || v == "browser" || v == "launch") {
        externalAuthRedirectHandler = RedirectHandler::Open;
      } else if (v == "print" || v == "log" || v == "none") {
        externalAuthRedirectHandler = RedirectHandler::Print;
      } else {
        if (error) *error = invalidValue(spec->canonical, value, "open or print");
        return false;
      }
      markSet(spec->canonical);
      return true;
    }
    case Key::Source: {
      if (trim(value).empty()) return true;  // keep the default rather than sending an empty source
      return setString(source);
    }
    case Key::ClientInfo: return setString(clientInfo);
    case Key::ClientTags: {
      clientTags.clear();
      for (const auto& part : split(value, ',')) {
        for (const auto& tag : split(part, ';')) clientTags.push_back(tag);
      }
      markSet(spec->canonical);
      return true;
    }
    case Key::TraceToken: return setString(traceToken);
    case Key::TimeZone: return setString(timeZone);
    case Key::Locale: return setString(locale);
    case Key::SessionProperties: return setMap(sessionProperties, true);
    case Key::ExtraCredentials: return setMap(extraCredentials, true);
    case Key::Roles: {
      // Entries: catalog=ROLE{name}, catalog=name, catalog=ALL, catalog=NONE; a bare value applies to
      // the system role.
      roles.clear();
      std::string current;
      auto flush = [&]() {
        const std::string entry(trim(current));  // copy: `current` is reused for the next entry
        current.clear();
        if (entry.empty()) return;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
          roles["system"] = normalizeRole(entry);
        } else {
          const std::string cat(trim(std::string_view(entry).substr(0, eq)));
          roles[cat.empty() ? std::string("system") : cat] = normalizeRole(std::string_view(entry).substr(eq + 1));
        }
      };
      for (const char c : value) {
        if (c == ';' || c == ',') flush();
        else current += c;
      }
      flush();
      markSet(spec->canonical);
      return true;
    }
    case Key::SessionUser: return setString(sessionUser);
    case Key::HttpProxy: return setString(httpProxy);
    case Key::SocksProxy: return setString(socksProxy);
    case Key::ProxyUser: return setString(proxyUser);
    case Key::ProxyPassword: return setSecret(proxyPassword);
    case Key::ConnectTimeout: return setInt(connectTimeoutSec, 0, 86400);
    case Key::RequestTimeout: return setInt(requestTimeoutSec, 0, 86400 * 7);
    case Key::QueryTimeout: return setInt(queryTimeoutSec, 0, 86400 * 30);
    case Key::IdleFetchTimeout: return setInt(idleFetchTimeoutSec, 0, 86400 * 30);
    case Key::MaxRetries: return setInt(maxRetries, 0, 1000);
    case Key::Compression: return setBool(compression);
    case Key::Spooling: return setBool(spooling);
    case Key::QueryDataEncoding: {
      if (trim(value).empty()) return true;
      std::vector<std::string> codecs;
      for (const auto& part : split(value, ',')) {
        const std::string codec = toLower(part);
        if (codec != "json" && codec != "json+zstd" && codec != "json+lz4") {
          if (error) *error = invalidValue(spec->canonical, value, "a comma list of json, json+zstd, json+lz4");
          return false;
        }
        if (std::find(codecs.begin(), codecs.end(), codec) == codecs.end()) codecs.push_back(codec);
      }
      queryDataEncoding = join(codecs, ",");
      markSet(spec->canonical);
      return true;
    }
    case Key::TargetResultSize: {
      if (trim(value).empty()) return true;
      int64_t v = 0;
      if (!parseIntField(spec->canonical, value, 1, 1LL << 20, &v, error)) return false;
      if (v > kMaxTargetResultSizeMb) {
        TRINO_LOG_WARN(kLogComponent, "TargetResultSize %lld MB exceeds the maximum; using %d MB",
                       static_cast<long long>(v), kMaxTargetResultSizeMb);
        v = kMaxTargetResultSizeMb;
      }
      targetResultSizeMb = static_cast<int>(v);
      markSet(spec->canonical);
      return true;
    }
    case Key::PrefetchPages: return setInt(prefetchPages, 1, 100000);
    case Key::MaxPrefetchBytes: {
      if (trim(value).empty()) return true;
      int64_t mb = 0;
      if (!parseIntField(spec->canonical, value, 1, 1LL << 20, &mb, error)) return false;
      maxPrefetchBytes = mb * kMegabyte;
      markSet(spec->canonical);
      return true;
    }
    case Key::MaxParallelSegments: return setInt(maxParallelSegments, 1, 256);
    case Key::UseUnicodeSqlCharacterTypes: return setBool(unicodeTypes);
    case Key::DefaultStringColumnLength: return setInt(defaultStringLength, 1, 2147483647);
    case Key::UnboundedVarcharAsLongVarchar: return setBool(unboundedVarcharAsLong);
    case Key::TimestampWithTimeZoneMode: {
      if (trim(value).empty()) return true;
      int mode = 0;
      if (!parseTzModeWord(value, &mode)) {
        if (error) *error = invalidValue(spec->canonical, value, "session, utc, wallclock or string");
        return false;
      }
      timestampTzMode = mode == 0   ? TimestampTzMode::Utc
                        : mode == 1 ? TimestampTzMode::WallClock
                        : mode == 2 ? TimestampTzMode::String
                                    : TimestampTzMode::Session;
      markSet(spec->canonical);
      return true;
    }
    case Key::TimeWithTimeZoneMode: {
      if (trim(value).empty()) return true;
      int mode = 0;
      if (!parseTzModeWord(value, &mode)) {
        if (error) *error = invalidValue(spec->canonical, value, "string, session, utc or wallclock");
        return false;
      }
      timeTzMode = mode == 0   ? TimeTzMode::Utc
                   : mode == 1 ? TimeTzMode::WallClock
                   : mode == 2 ? TimeTzMode::String
                               : TimeTzMode::Session;
      markSet(spec->canonical);
      return true;
    }
    case Key::IntervalMode: {
      const std::string v = toLower(trim(value));
      if (v.empty()) return true;
      if (v == "string" || v == "text") {
        intervalMode = IntervalMode::String;
      } else if (v == "interval" || v == "native" || v == "struct") {
        intervalMode = IntervalMode::Interval;
      } else {
        if (error) *error = invalidValue(spec->canonical, value, "string or interval");
        return false;
      }
      markSet(spec->canonical);
      return true;
    }
    case Key::UuidAsGuid: return setBool(uuidAsGuid);
    case Key::ComplexTypesAsJson: return setBool(complexTypesAsJson);
    case Key::DecimalAsDouble: return setBool(decimalAsDouble);
    case Key::ExplicitPrepare: return setBool(explicitPrepare);
    case Key::MetadataCaseSensitive: return setBool(metadataCaseSensitive);
    case Key::EnableTransactions: return setBool(enableTransactions);
    case Key::ValidateConnection: return setBool(validateConnection);
    case Key::LogLevel: {
      if (trim(value).empty()) return true;
      int64_t v = 0;
      if (!parseInt64(trim(value), &v)) {
        // Accept the level names too (error/warn/info/debug/trace/off) for hand-written DSNs.
        const std::string name = toLower(trim(value));
        if (name == "off" || name == "none") v = 0;
        else if (name == "error") v = 1;
        else if (name == "warn" || name == "warning") v = 2;
        else if (name == "info") v = 3;
        else if (name == "debug") v = 4;
        else if (name == "trace") v = 5;
        else {
          if (error) *error = invalidValue(spec->canonical, value, "0..5");
          return false;
        }
      }
      if (v < 0) v = 0;
      if (v > 5) v = 5;
      logLevel = static_cast<int>(v);
      markSet(spec->canonical);
      return true;
    }
    case Key::LogPath: return setString(logPath);
    case Key::ExtraHeaders: {
      // `Name:value;Name2:value2` — values may contain commas, so only ';' separates entries.
      // Header values are typically Authorization tokens / API keys: errors name the entry, never the value.
      std::vector<std::pair<std::string, std::string>> headers;
      size_t entryIndex = 0;
      for (const auto& entry : split(value, ';')) {
        ++entryIndex;
        size_t sep = entry.find(':');
        if (sep == std::string::npos) sep = entry.find('=');
        if (sep == std::string::npos) {
          if (error) *error = invalidEntry(spec->canonical, entryIndex, "entries of the form Name:value");
          return false;
        }
        const std::string name(trim(std::string_view(entry).substr(0, sep)));
        if (name.empty() || name.find_first_of(" \t\r\n") != std::string::npos) {
          if (error) *error = invalidEntry(spec->canonical, entryIndex, "valid HTTP header names");
          return false;
        }
        headers.emplace_back(name, std::string(trim(std::string_view(entry).substr(sep + 1))));
      }
      extraHeaders = std::move(headers);
      markSet(spec->canonical);
      return true;
    }
    case Key::AnsiEncodingKey: {
      const std::string v = toLower(trim(value));
      if (v.empty()) return true;
      if (v == "utf8" || v == "utf-8" || v == "unicode") {
        ansiEncoding = AnsiEncoding::Utf8;
      } else if (v == "system" || v == "ansi" || v == "native" || v == "locale") {
        ansiEncoding = AnsiEncoding::System;
      } else {
        if (error) *error = invalidValue(spec->canonical, value, "utf8 or system");
        return false;
      }
      markSet(spec->canonical);
      return true;
    }
    case Key::ReadOnly: return setBool(readOnly);
  }
  if (error) *error = "Unhandled connection option";
  return false;
}

void ConnectionConfig::applyAll(const KeyValueMap& kv) {
  for (const auto& [key, value] : kv) {
    std::string error;
    if (!applyKeyValue(key, value, &error)) {
      throw DriverError(sqlstate::kGeneralError, error);
    }
  }
}

void ConnectionConfig::loadDsn(std::string_view dsnNameIn, bool onlyMissing) {
  const std::string dsnName(trim(dsnNameIn));
  if (dsnName.empty()) return;

  auto isSet = [this](std::string_view canonical) {
    for (const auto& k : setKeys_) {
      if (iequals(k, canonical)) return true;
    }
    return false;
  };

  // Preferred path: enumerate every entry of the section. Some installer libraries do not support
  // enumeration (NULL entry), in which case we probe every known key and alias instead.
  std::vector<std::pair<std::string, std::string>> entries;
  std::vector<std::string> keys = enumerateDsnKeys(dsnName);
  const bool enumerated = !keys.empty();
  if (!enumerated) {
    for (const auto& spec : kKeys) {
      keys.emplace_back(spec.canonical);
      for (const char* const* alias = spec.aliases; *alias != nullptr; ++alias) keys.emplace_back(*alias);
    }
  }
  for (const auto& key : keys) {
    if (iequals(key, "DSN")) continue;  // the section name is the DSN; never stored as an entry
    std::optional<std::string> value = readDsnValue(dsnName, key);
    if (!value) {
      if (enumerated) entries.emplace_back(key, std::string());  // present but empty
      continue;
    }
    entries.emplace_back(key, std::move(*value));
  }
  if (entries.empty()) {
    throw DriverError(sqlstate::kDataSourceNotFound, "Data source name not found: '" + dsnName + "'");
  }

  for (const auto& [key, value] : entries) {
    const std::string canonical = canonicalKey(key);
    if (onlyMissing) {
      if (canonical.empty()) {
        if (unknown.find(key) != unknown.end()) continue;
      } else {
        if (isSet(canonical)) continue;
        // The Simba compatibility flags rewrite SSLVerification: an explicit SSLVerification from the
        // connection string must not be undone by a DSN flag.
        if ((iequals(canonical, "AllowSelfSignedServerCert") || iequals(canonical, "AllowHostNameCNMismatch")) &&
            isSet("SSLVerification")) {
          continue;
        }
      }
    }
    std::string error;
    if (!applyKeyValue(key, value, &error)) {
      throw DriverError(sqlstate::kGeneralError, "DSN '" + dsnName + "': " + error);
    }
  }
  if (dsn.empty()) dsn = dsnName;
  TRINO_LOG_DEBUG(kLogComponent, "loaded %zu entries from DSN '%s'%s", entries.size(), dsnName.c_str(),
                  enumerated ? "" : " (probed keys)");
}

// ---- derived defaults and validation ------------------------------------------------------------

void ConnectionConfig::finalize() {
  auto isSet = [this](std::string_view canonical) {
    for (const auto& k : setKeys_) {
      if (iequals(k, canonical)) return true;
    }
    return false;
  };

  if (!authExplicit) {
    if (!password.empty()) auth = AuthType::Ldap;
    else if (!accessToken.empty() || !accessTokenFile.empty()) auth = AuthType::Jwt;
    else auth = AuthType::None;
  }
  // TLS is implied by the standard HTTPS port and by every authenticated mode; only an explicit
  // SSL=0 turns it off.
  if (!sslExplicit) {
    // Credentials imply TLS unless the user explicitly accepted an insecure transport.
    ssl = (port == 443) || (auth != AuthType::None && !allowInsecurePassword);
  }
  if (!isSet("Port") || port == 0) {
    port = ssl ? 443 : 8080;
  }
  if (source.empty()) source = "trino-odbc";
  if (kerberosServiceName.empty()) kerberosServiceName = "HTTP";
  if (queryDataEncoding.empty()) queryDataEncoding = "json+zstd,json+lz4,json";
  // Project default: Turkey time unless the DSN / connection string names a zone (or "system").
  const std::string_view tz = trim(timeZone);
  if (tz.empty()) {
    timeZone = kDefaultSessionTimeZone;
  } else if (iequals(tz, kSystemTimeZoneKeyword)) {
    timeZone = kSystemTimeZoneKeyword;
  } else if (tz.size() != timeZone.size()) {
    timeZone = std::string(tz);
  }
}

void ConnectionConfig::validate() const {
  if (trim(host).empty()) {
    throw DriverError(sqlstate::kGeneralError, "Host is required (set Host=<coordinator host> in the DSN or connection string)");
  }
  if (port < 1 || port > 65535) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "Port must be between 1 and 65535");
  }

  const bool insecureTransport = !ssl;
  switch (auth) {
    case AuthType::None:
      break;
    case AuthType::Ldap:
      if (user.empty() || password.empty()) {
        throw DriverError(sqlstate::kInvalidAuth, "LDAP/password authentication requires UID and PWD");
      }
      if (insecureTransport && !allowInsecurePassword) {
        throw DriverError(sqlstate::kInvalidAuth,
                          "Password authentication requires SSL (set SSL=1, or AllowInsecurePassword=1 to send the password over plain HTTP)");
      }
      break;
    case AuthType::Jwt:
      if (accessToken.empty() && accessTokenFile.empty()) {
        throw DriverError(sqlstate::kInvalidAuth, "JWT authentication requires AccessToken or AccessTokenFile");
      }
      if (insecureTransport && !allowInsecurePassword) {
        throw DriverError(sqlstate::kInvalidAuth, "Token authentication requires SSL (set SSL=1, or AllowInsecurePassword=1 to send the token over plain HTTP)");
      }
      break;
    case AuthType::Certificate:
      if (clientCert.empty()) {
        throw DriverError(sqlstate::kInvalidAuth, "Certificate authentication requires ClientCert");
      }
      if (insecureTransport) {
        throw DriverError(sqlstate::kInvalidAuth, "Certificate authentication requires SSL");
      }
      break;
    case AuthType::Kerberos:
    case AuthType::OAuth2:
      if (insecureTransport && !allowInsecurePassword) {
        throw DriverError(sqlstate::kInvalidAuth,
                          std::string(authTypeName(auth)) + " authentication requires SSL (set SSL=1, or AllowInsecurePassword=1 for an insecure development setup)");
      }
      break;
  }
  if (needsUser()) {
    throw DriverError(sqlstate::kInvalidAuth, "User name is required (UID)");
  }

  if (minTlsVersion != "1.2" && minTlsVersion != "1.3") {
    throw DriverError(sqlstate::kInvalidAttributeValue, "MinTLSVersion must be 1.2 or 1.3");
  }
  if (!httpProxy.empty() && !socksProxy.empty()) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "HttpProxy and SocksProxy are mutually exclusive");
  }
  if (connectTimeoutSec < 0 || requestTimeoutSec < 0 || queryTimeoutSec < 0 || idleFetchTimeoutSec < 0 ||
      externalAuthTimeoutSec < 0) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "Timeouts must not be negative");
  }
  if (maxRetries < 0) throw DriverError(sqlstate::kInvalidAttributeValue, "MaxRetries must not be negative");
  if (targetResultSizeMb < 1 || targetResultSizeMb > kMaxTargetResultSizeMb) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "TargetResultSize must be between 1 and 128 (MB)");
  }
  if (prefetchPages < 1) throw DriverError(sqlstate::kInvalidAttributeValue, "PrefetchPages must be at least 1");
  if (maxPrefetchBytes < kMegabyte) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "MaxPrefetchBytes must be at least 1 (MB)");
  }
  if (maxParallelSegments < 1) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "MaxParallelSegments must be at least 1");
  }
  if (defaultStringLength < 1) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "DefaultStringColumnLength must be at least 1");
  }
  if (logLevel < 0 || logLevel > 5) {
    throw DriverError(sqlstate::kInvalidAttributeValue, "LogLevel must be between 0 and 5");
  }
  for (const auto& codec : split(queryDataEncoding, ',')) {
    const std::string c = toLower(codec);
    if (c != "json" && c != "json+zstd" && c != "json+lz4") {
      throw DriverError(sqlstate::kInvalidAttributeValue, "QueryDataEncoding contains an unknown encoding: " + codec);
    }
  }
  for (const auto& [name, value] : extraHeaders) {
    (void)value;
    if (name.empty()) throw DriverError(sqlstate::kInvalidAttributeValue, "ExtraHeaders contains an empty header name");
  }

  if (ssl && sslVerification == SslVerification::None) {
    TRINO_LOG_WARN(kLogComponent,
                   "SSLVerification=None: the server certificate of %s is not verified (vulnerable to interception)",
                   host.c_str());
  }
}

// ---- output ----------------------------------------------------------------------------------------

std::string ConnectionConfig::toConnectionString(bool includeSecrets) const {
  auto isSet = [this](const char* canonical) {
    for (const auto& k : setKeys_) {
      if (iequals(k, canonical)) return true;
    }
    return false;
  };

  std::string out;
  auto emit = [&](const char* key, std::string value, bool secret) {
    if (!out.empty()) out += ';';
    out += key;
    out += '=';
    // The mask is emitted verbatim (never brace-quoted) so logs show a recognisable "PWD=***".
    if (secret && !includeSecrets) out += "***";
    else out += quoteConnValue(value);
  };

  for (const auto& spec : kKeys) {
    std::string value;
    bool always = false;  // emitted even when not explicitly given (needed to reconnect)
    switch (spec.id) {
      case Key::Dsn:
        // An empty DSN (e.g. a named DSN that did not exist and was dropped in favour of DRIVER=)
        // must not be emitted: "DSN=;" would make a driver manager look up the default data source.
        if (dsn.empty()) continue;
        value = dsn;
        break;
      case Key::Driver: value = driver; break;
      case Key::Description: value = description; break;
      case Key::Host: value = host; always = true; break;
      case Key::Port: value = port > 0 ? std::to_string(port) : ""; always = port > 0; break;
      case Key::Catalog: value = catalog; break;
      case Key::Schema: value = schema; break;
      case Key::Uid: value = user; always = !user.empty(); break;
      case Key::Pwd: value = password; break;
      case Key::AuthenticationType: value = authTypeName(auth); always = auth != AuthType::None; break;
      case Key::Ssl: value = boolText(ssl); always = true; break;
      case Key::SslVerification: value = sslVerificationName(sslVerification); break;
      case Key::AllowSelfSignedServerCert:
      case Key::AllowHostNameCNMismatch:
        continue;  // already folded into SSLVerification
      case Key::TrustedCerts: value = caFile; break;
      case Key::CaPath: value = caPath; break;
      case Key::UseSystemTrustStore: value = boolText(useSystemTrustStore); break;
      case Key::ClientCert: value = clientCert; break;
      case Key::ClientKey: value = clientKey; break;
      case Key::ClientKeyPassword: value = clientKeyPassword; break;
      case Key::MinTlsVersion: value = minTlsVersion; break;
      case Key::AllowInsecurePassword: value = boolText(allowInsecurePassword); break;
      case Key::AccessToken: value = accessToken; break;
      case Key::AccessTokenFile: value = accessTokenFile; break;
      case Key::KerberosServiceName: value = kerberosServiceName; break;
      case Key::KerberosDelegation: value = boolText(kerberosDelegation); break;
      case Key::KerberosUseCanonicalHostname: value = boolText(kerberosUseCanonicalHostname); break;
      case Key::ExternalAuthTimeout: value = std::to_string(externalAuthTimeoutSec); break;
      case Key::ExternalAuthRedirectHandler:
        value = externalAuthRedirectHandler == RedirectHandler::Open ? "open" : "print";
        break;
      case Key::Source: value = source; break;
      case Key::ClientInfo: value = clientInfo; break;
      case Key::ClientTags: value = join(clientTags, ","); break;
      case Key::TraceToken: value = traceToken; break;
      case Key::TimeZone: value = timeZone; break;
      case Key::Locale: value = locale; break;
      case Key::SessionProperties: value = joinPairs(sessionProperties); break;
      case Key::ExtraCredentials: value = joinPairs(extraCredentials); break;
      case Key::Roles: value = joinPairs(roles); break;
      case Key::SessionUser: value = sessionUser; break;
      case Key::HttpProxy: value = httpProxy; break;
      case Key::SocksProxy: value = socksProxy; break;
      case Key::ProxyUser: value = proxyUser; break;
      case Key::ProxyPassword: value = proxyPassword; break;
      case Key::ConnectTimeout: value = std::to_string(connectTimeoutSec); break;
      case Key::RequestTimeout: value = std::to_string(requestTimeoutSec); break;
      case Key::QueryTimeout: value = std::to_string(queryTimeoutSec); break;
      case Key::IdleFetchTimeout: value = std::to_string(idleFetchTimeoutSec); break;
      case Key::MaxRetries: value = std::to_string(maxRetries); break;
      case Key::Compression: value = boolText(compression); break;
      case Key::Spooling: value = boolText(spooling); break;
      case Key::QueryDataEncoding: value = queryDataEncoding; break;
      case Key::TargetResultSize: value = std::to_string(targetResultSizeMb); break;
      case Key::PrefetchPages: value = std::to_string(prefetchPages); break;
      case Key::MaxPrefetchBytes: value = std::to_string(maxPrefetchBytes / kMegabyte); break;
      case Key::MaxParallelSegments: value = std::to_string(maxParallelSegments); break;
      case Key::UseUnicodeSqlCharacterTypes: value = boolText(unicodeTypes); break;
      case Key::DefaultStringColumnLength: value = std::to_string(defaultStringLength); break;
      case Key::UnboundedVarcharAsLongVarchar: value = boolText(unboundedVarcharAsLong); break;
      case Key::TimestampWithTimeZoneMode: value = timestampTzModeName(timestampTzMode); break;
      case Key::TimeWithTimeZoneMode: value = timeTzModeName(timeTzMode); break;
      case Key::IntervalMode: value = intervalMode == IntervalMode::String ? "string" : "interval"; break;
      case Key::UuidAsGuid: value = boolText(uuidAsGuid); break;
      case Key::ComplexTypesAsJson: value = boolText(complexTypesAsJson); break;
      case Key::DecimalAsDouble: value = boolText(decimalAsDouble); break;
      case Key::ExplicitPrepare: value = boolText(explicitPrepare); break;
      case Key::MetadataCaseSensitive: value = boolText(metadataCaseSensitive); break;
      case Key::EnableTransactions: value = boolText(enableTransactions); break;
      case Key::ValidateConnection: value = boolText(validateConnection); break;
      case Key::LogLevel: value = std::to_string(logLevel); break;
      case Key::LogPath: value = logPath; break;
      case Key::ExtraHeaders: {
        std::vector<std::string> parts;
        for (const auto& [n, v] : extraHeaders) parts.push_back(n + ":" + v);
        value = join(parts, ";");
        break;
      }
      case Key::AnsiEncodingKey: value = ansiEncoding == AnsiEncoding::Utf8 ? "utf8" : "system"; break;
      case Key::ReadOnly: value = boolText(readOnly); break;
    }
    if (always || isSet(spec.canonical)) emit(spec.canonical, std::move(value), spec.secret);
  }
  for (const auto& [key, value] : unknown) emit(key.c_str(), value, false);
  return out;
}

std::string ConnectionConfig::baseUrl() const {
  std::string url = ssl ? "https://" : "http://";
  // A bare IPv6 literal must be bracketed inside a URL.
  if (host.find(':') != std::string::npos && host.front() != '[') {
    url += '[';
    url += host;
    url += ']';
  } else {
    url += host;
  }
  url += ':';
  url += std::to_string(port);
  return url;
}

TypeMappingOptions ConnectionConfig::typeMapping() const {
  TypeMappingOptions o;
  o.unicode = unicodeTypes;
  o.defaultStringLength = defaultStringLength;
  o.unboundedAsLong = unboundedVarcharAsLong;
  o.timestampTz = timestampTzMode;
  o.timeTz = timeTzMode;
  o.interval = intervalMode;
  o.uuidAsGuid = uuidAsGuid;
  o.decimalAsDouble = decimalAsDouble;
  return o;
}

bool ConnectionConfig::needsUser() const {
  // Kerberos, OAuth2 and client certificates identify the user on the server side.
  if (!user.empty()) return false;
  return auth == AuthType::None || auth == AuthType::Ldap || auth == AuthType::Jwt;
}

}  // namespace trino_odbc
