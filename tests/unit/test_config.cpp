// Unit tests for core/config: connection-string grammar, key aliases, derived defaults, validation
// SQLSTATEs and canonical serialisation.
#include "unit/test_framework.h"

#include "core/config.h"
#include "util/errors.h"
#include "util/tz.h"

#include <string>

using namespace trino_odbc;

namespace {

/// Parses + applies a connection string and runs finalize() (not validate()).
ConnectionConfig configFrom(const std::string& connStr) {
  ConnectionConfig c;
  c.applyAll(ConnectionConfig::parseConnectionString(connStr));
  c.finalize();
  return c;
}

/// Returns the SQLSTATE thrown by validate(), or "" when it does not throw.
std::string validateState(const ConnectionConfig& c) {
  try {
    c.validate();
  } catch (const DriverError& e) {
    return e.sqlstate();
  }
  return "";
}

}  // namespace

// ---- connection-string grammar --------------------------------------------------------------------

TEST_CASE("config: parse simple key=value pairs") {
  auto kv = ConnectionConfig::parseConnectionString("Host=trino.example.com;Port=8443;UID=alice");
  CHECK_EQ(kv.size(), size_t(3));
  CHECK_EQ(kv["Host"], std::string("trino.example.com"));
  CHECK_EQ(kv["Port"], std::string("8443"));
  CHECK_EQ(kv["UID"], std::string("alice"));
}

TEST_CASE("config: keys are case-insensitive and first occurrence wins") {
  auto kv = ConnectionConfig::parseConnectionString("host=a;HOST=b;Host=c");
  CHECK_EQ(kv.size(), size_t(1));
  CHECK_EQ(kv["HOST"], std::string("a"));
  CHECK_EQ(kv["host"], std::string("a"));
}

TEST_CASE("config: braces protect semicolons and '}}' escapes a brace") {
  auto kv = ConnectionConfig::parseConnectionString("PWD={p;a=ss}};w}ord};Host=h");
  CHECK_EQ(kv["PWD"], std::string("p;a=ss};w"));
  CHECK_EQ(kv["Host"], std::string("h"));
  // Text after the closing brace but before ';' is ignored.
  kv = ConnectionConfig::parseConnectionString("PWD={abc}junk;Host=h");
  CHECK_EQ(kv["PWD"], std::string("abc"));
  CHECK_EQ(kv["Host"], std::string("h"));
  // Unterminated brace runs to the end.
  kv = ConnectionConfig::parseConnectionString("PWD={abc;def");
  CHECK_EQ(kv["PWD"], std::string("abc;def"));
}

TEST_CASE("config: quoted values") {
  auto kv = ConnectionConfig::parseConnectionString("PWD=\"a;b\"\"c\";UID='x;y';Host=h");
  CHECK_EQ(kv["PWD"], std::string("a;b\"c"));
  CHECK_EQ(kv["UID"], std::string("x;y"));
  CHECK_EQ(kv["Host"], std::string("h"));
}

TEST_CASE("config: whitespace, empty entries and malformed fragments") {
  auto kv = ConnectionConfig::parseConnectionString("  Host = h1 ;; ; Port= 80 ;garbage;=novalue;Catalog=;Schema=  s ");
  CHECK_EQ(kv["Host"], std::string("h1"));
  CHECK_EQ(kv["Port"], std::string("80"));
  CHECK(kv.find("garbage") == kv.end());
  CHECK_EQ(kv.count("Catalog"), size_t(1));
  CHECK_EQ(kv["Catalog"], std::string(""));
  CHECK_EQ(kv["Schema"], std::string("s"));
  CHECK_EQ(ConnectionConfig::parseConnectionString("").size(), size_t(0));
  CHECK_EQ(ConnectionConfig::parseConnectionString(";;;").size(), size_t(0));
}

TEST_CASE("config: value may contain '=' after the first one") {
  auto kv = ConnectionConfig::parseConnectionString("SessionProperties=a=1,b=2;Host=h");
  CHECK_EQ(kv["SessionProperties"], std::string("a=1,b=2"));
  CHECK_EQ(kv["Host"], std::string("h"));
}

TEST_CASE("config: braces are only special at the start of a value") {
  auto kv = ConnectionConfig::parseConnectionString("Roles=system=ROLE{admin},tpch=ALL;Host=h");
  CHECK_EQ(kv["Roles"], std::string("system=ROLE{admin},tpch=ALL"));
  CHECK_EQ(kv["Host"], std::string("h"));
}

// ---- key table -----------------------------------------------------------------------------------

TEST_CASE("config: knownKeys, canonicalKey and isSecretKey") {
  const auto& keys = ConnectionConfig::knownKeys();
  CHECK(keys.size() > 70);
  CHECK_EQ(keys.front(), std::string("DSN"));
  CHECK_EQ(keys[3], std::string("Host"));
  CHECK_EQ(ConnectionConfig::canonicalKey("server"), std::string("Host"));
  CHECK_EQ(ConnectionConfig::canonicalKey("HOSTNAME"), std::string("Host"));
  CHECK_EQ(ConnectionConfig::canonicalKey("database"), std::string("Catalog"));
  CHECK_EQ(ConnectionConfig::canonicalKey("password"), std::string("PWD"));
  CHECK_EQ(ConnectionConfig::canonicalKey("AuthMech"), std::string("AuthenticationType"));
  CHECK_EQ(ConnectionConfig::canonicalKey("Encrypt"), std::string("SSL"));
  CHECK_EQ(ConnectionConfig::canonicalKey("SSLTrustStorePath"), std::string("TrustedCerts"));
  CHECK_EQ(ConnectionConfig::canonicalKey("jwt"), std::string("AccessToken"));
  CHECK_EQ(ConnectionConfig::canonicalKey("LoginTimeout"), std::string("ConnectTimeout"));
  CHECK_EQ(ConnectionConfig::canonicalKey("PrefetchBufferMB"), std::string("MaxPrefetchBytes"));
  CHECK_EQ(ConnectionConfig::canonicalKey("AssumeLiteralNamesInMetadataCalls"), std::string("MetadataCaseSensitive"));
  CHECK_EQ(ConnectionConfig::canonicalKey("  Port  "), std::string("Port"));
  CHECK_EQ(ConnectionConfig::canonicalKey("NoSuchKey"), std::string(""));
  CHECK_EQ(ConnectionConfig::canonicalKey(""), std::string(""));
  CHECK(ConnectionConfig::isSecretKey("PWD"));
  CHECK(ConnectionConfig::isSecretKey("password"));
  CHECK(ConnectionConfig::isSecretKey("AccessToken"));
  CHECK(ConnectionConfig::isSecretKey("ClientKeyPassword"));
  CHECK(ConnectionConfig::isSecretKey("ProxyPassword"));
  CHECK(ConnectionConfig::isSecretKey("ExtraCredentials"));
  CHECK(!ConnectionConfig::isSecretKey("Host"));
  CHECK(!ConnectionConfig::isSecretKey("UID"));
  CHECK(!ConnectionConfig::isSecretKey("Unknown"));
  // Every canonical key resolves to itself.
  for (const auto& k : keys) CHECK_EQ(ConnectionConfig::canonicalKey(k), k);
}

// ---- defaults and aliases -----------------------------------------------------------------------

TEST_CASE("config: defaults with only Host") {
  ConnectionConfig c = configFrom("Host=h");
  CHECK_EQ(c.host, std::string("h"));
  CHECK_EQ(c.port, 8080);
  CHECK(!c.ssl);
  CHECK(c.auth == AuthType::None);
  CHECK(c.sslVerification == SslVerification::Full);
  CHECK(c.useSystemTrustStore);
  CHECK_EQ(c.minTlsVersion, std::string("1.2"));
  CHECK_EQ(c.source, std::string("trino-odbc"));
  CHECK_EQ(c.kerberosServiceName, std::string("HTTP"));
  CHECK_EQ(c.connectTimeoutSec, 30);
  CHECK_EQ(c.requestTimeoutSec, 60);
  CHECK_EQ(c.queryTimeoutSec, 0);
  CHECK_EQ(c.maxRetries, 5);
  CHECK(c.compression);
  CHECK(c.spooling);
  CHECK_EQ(c.queryDataEncoding, std::string("json+zstd,json+lz4,json"));
  CHECK_EQ(c.targetResultSizeMb, 16);
  CHECK_EQ(c.prefetchPages, 4);
  CHECK_EQ(c.maxPrefetchBytes, int64_t(64) * 1024 * 1024);
  CHECK_EQ(c.maxParallelSegments, 4);
  CHECK(c.unicodeTypes);
  CHECK_EQ(c.defaultStringLength, 65535);
  CHECK(c.timestampTzMode == TimestampTzMode::Session);  // project default: session zone
  CHECK(c.timeTzMode == TimeTzMode::String);
  CHECK_EQ(c.timeZone, std::string("Europe/Istanbul"));   // project default session zone
  CHECK(c.intervalMode == IntervalMode::String);
  CHECK(!c.uuidAsGuid);
  CHECK(!c.decimalAsDouble);
  CHECK(!c.explicitPrepare);
  CHECK(!c.enableTransactions);
  CHECK(c.validateConnection);
  CHECK(!c.readOnly);
  CHECK_EQ(c.logLevel, 0);
  CHECK(c.unknown.empty());
  CHECK_EQ(c.baseUrl(), std::string("http://h:8080"));
}

TEST_CASE("config: aliases map to canonical fields") {
  ConnectionConfig c = configFrom(
      "Server=srv;Database=cat;User=u;Password=p;UseSSL=1;SSLMode=CA;SSLCA=/ca.pem;SSLCAPath=/cadir;"
      "SSLCert=/c.pem;SSLKey=/k.pem;SSLKeyStorePassword=kp;KrbServiceName=trino;OAuth2Timeout=5;"
      "ApplicationName=app;TimeZoneID=Europe/Berlin;Language=tr-TR;Proxy=proxy:3128;LoginTimeout=7;"
      "Timeout=8;Retries=2;EnableCompression=0;SpoolingEnabled=0;PrefetchBufferMB=8;UnicodeTypes=0;"
      "MaxVarcharSize=100;TimestampTZMode=wallclock;AssumeLiteralNamesInMetadataCalls=1");
  CHECK_EQ(c.host, std::string("srv"));
  CHECK_EQ(c.catalog, std::string("cat"));
  CHECK_EQ(c.user, std::string("u"));
  CHECK_EQ(c.password, std::string("p"));
  CHECK(c.ssl);
  CHECK(c.sslExplicit);
  CHECK(c.sslVerification == SslVerification::Ca);
  CHECK_EQ(c.caFile, std::string("/ca.pem"));
  CHECK_EQ(c.caPath, std::string("/cadir"));
  CHECK_EQ(c.clientCert, std::string("/c.pem"));
  CHECK_EQ(c.clientKey, std::string("/k.pem"));
  CHECK_EQ(c.clientKeyPassword, std::string("kp"));
  CHECK_EQ(c.kerberosServiceName, std::string("trino"));
  CHECK_EQ(c.externalAuthTimeoutSec, 5);
  CHECK_EQ(c.source, std::string("app"));
  CHECK_EQ(c.timeZone, std::string("Europe/Berlin"));
  CHECK_EQ(c.locale, std::string("tr-TR"));
  CHECK_EQ(c.httpProxy, std::string("proxy:3128"));
  CHECK_EQ(c.connectTimeoutSec, 7);
  CHECK_EQ(c.requestTimeoutSec, 8);
  CHECK_EQ(c.maxRetries, 2);
  CHECK(!c.compression);
  CHECK(!c.spooling);
  CHECK_EQ(c.maxPrefetchBytes, int64_t(8) * 1024 * 1024);
  CHECK(!c.unicodeTypes);
  CHECK_EQ(c.defaultStringLength, 100);
  CHECK(c.timestampTzMode == TimestampTzMode::WallClock);
  CHECK(c.metadataCaseSensitive);
  // PWD given -> LDAP inferred, SSL explicit.
  CHECK(c.auth == AuthType::Ldap);
  CHECK(!c.authExplicit);
  CHECK_EQ(c.port, 443);
}

TEST_CASE("config: booleans accept every spelling") {
  for (const char* t : {"1", "true", "TRUE", "yes", "on", "Y", "t"}) {
    ConnectionConfig c = configFrom(std::string("Host=h;ReadOnly=") + t);
    CHECK(c.readOnly);
  }
  for (const char* f : {"0", "false", "No", "off", "n", "F"}) {
    ConnectionConfig c = configFrom(std::string("Host=h;ReadOnly=") + f + ";UuidAsGuid=1");
    CHECK(!c.readOnly);
    CHECK(c.uuidAsGuid);
  }
  ConnectionConfig c;
  std::string err;
  CHECK(!c.applyKeyValue("ReadOnly", "maybe", &err));
  CHECK(err.find("ReadOnly") != std::string::npos);
  CHECK_THROWS_AS(configFrom("Host=h;Compression=maybe"), DriverError);
}

TEST_CASE("config: AuthenticationType names and numbers") {
  struct Case { const char* text; AuthType expected; };
  const Case cases[] = {
      {"None", AuthType::None}, {"0", AuthType::None},
      {"LDAP", AuthType::Ldap}, {"ldap", AuthType::Ldap}, {"1", AuthType::Ldap}, {"Password", AuthType::Ldap}, {"Basic", AuthType::Ldap},
      {"Kerberos", AuthType::Kerberos}, {"2", AuthType::Kerberos},
      {"JWT", AuthType::Jwt}, {"3", AuthType::Jwt}, {"Token", AuthType::Jwt}, {"AccessToken", AuthType::Jwt},
      {"OAuth2", AuthType::OAuth2}, {"4", AuthType::OAuth2}, {"ExternalAuthentication", AuthType::OAuth2}, {"External", AuthType::OAuth2},
      {"Certificate", AuthType::Certificate}, {"5", AuthType::Certificate},
  };
  for (const auto& cs : cases) {
    AuthType t = AuthType::None;
    CHECK(parseAuthType(cs.text, &t));
    CHECK(t == cs.expected);
    ConnectionConfig c = configFrom(std::string("Host=h;AuthMech=") + cs.text);
    CHECK(c.auth == cs.expected);
    CHECK(c.authExplicit);
  }
  AuthType t = AuthType::None;
  CHECK(!parseAuthType("magic", &t));
  CHECK_THROWS_AS(configFrom("Host=h;AuthenticationType=magic"), DriverError);
  CHECK_EQ(std::string(authTypeName(AuthType::Ldap)), std::string("LDAP"));
  CHECK_EQ(std::string(authTypeName(AuthType::OAuth2)), std::string("OAuth2"));
}

TEST_CASE("config: SSLVerification values and Simba aliases") {
  SslVerification v = SslVerification::Full;
  CHECK(parseSslVerification("full", &v));
  CHECK(v == SslVerification::Full);
  CHECK(parseSslVerification("CA", &v));
  CHECK(v == SslVerification::Ca);
  CHECK(parseSslVerification("none", &v));
  CHECK(v == SslVerification::None);
  CHECK(!parseSslVerification("partial", &v));
  CHECK_EQ(std::string(sslVerificationName(SslVerification::Ca)), std::string("CA"));

  CHECK(configFrom("Host=h;SSL=1;AllowSelfSignedServerCert=1").sslVerification == SslVerification::None);
  CHECK(configFrom("Host=h;SSL=1;AllowSelfSignedServerCert=0").sslVerification == SslVerification::Full);
  CHECK(configFrom("Host=h;SSL=1;AllowHostNameCNMismatch=1").sslVerification == SslVerification::Ca);
  // CN mismatch flag only downgrades Full; it never upgrades None.
  CHECK(configFrom("Host=h;SSL=1;SSLVerification=None;AllowHostNameCNMismatch=1").sslVerification == SslVerification::None);
  CHECK(configFrom("Host=h;SSL=1;SSLVerification=CA;AllowHostNameCNMismatch=1").sslVerification == SslVerification::Ca);
  CHECK_THROWS_AS(configFrom("Host=h;SSLVerification=partial"), DriverError);
}

// ---- finalize rules --------------------------------------------------------------------------------

TEST_CASE("config: finalize derives port, ssl and auth") {
  // SSL=1 without port -> 443.
  ConnectionConfig c = configFrom("Host=h;SSL=1");
  CHECK_EQ(c.port, 443);
  CHECK(c.ssl);
  CHECK_EQ(c.baseUrl(), std::string("https://h:443"));
  // Port 443 implies SSL.
  c = configFrom("Host=h;Port=443");
  CHECK(c.ssl);
  CHECK(!c.sslExplicit);
  // ... unless SSL=0 is explicit.
  c = configFrom("Host=h;Port=443;SSL=0");
  CHECK(!c.ssl);
  CHECK_EQ(c.port, 443);
  // Explicit port with SSL kept.
  c = configFrom("Host=h;Port=8443;SSL=1");
  CHECK_EQ(c.port, 8443);
  // PWD implies LDAP which implies SSL and port 443.
  c = configFrom("Host=h;UID=u;PWD=p");
  CHECK(c.auth == AuthType::Ldap);
  CHECK(c.ssl);
  CHECK_EQ(c.port, 443);
  // AccessToken implies JWT.
  c = configFrom("Host=h;AccessToken=tok");
  CHECK(c.auth == AuthType::Jwt);
  CHECK(c.ssl);
  c = configFrom("Host=h;AccessTokenFile=/tmp/token");
  CHECK(c.auth == AuthType::Jwt);
  // Explicit auth wins over inference.
  c = configFrom("Host=h;UID=u;PWD=p;AuthenticationType=None");
  CHECK(c.auth == AuthType::None);
  CHECK(!c.ssl);
  CHECK_EQ(c.port, 8080);
  // Kerberos implies SSL.
  c = configFrom("Host=h;AuthenticationType=Kerberos");
  CHECK(c.ssl);
  CHECK_EQ(c.port, 443);
  // Explicit SSL=0 with LDAP stays plain http on 8080.
  c = configFrom("Host=h;UID=u;PWD=p;SSL=0");
  CHECK(!c.ssl);
  CHECK_EQ(c.port, 8080);
  // finalize() is idempotent.
  c.finalize();
  CHECK(!c.ssl);
  CHECK_EQ(c.port, 8080);
}

TEST_CASE("config: needsUser and typeMapping") {
  CHECK(configFrom("Host=h").needsUser());
  CHECK(!configFrom("Host=h;UID=u").needsUser());
  CHECK(configFrom("Host=h;AccessToken=t").needsUser());
  CHECK(!configFrom("Host=h;AuthenticationType=Kerberos").needsUser());
  CHECK(!configFrom("Host=h;AuthenticationType=OAuth2").needsUser());
  CHECK(!configFrom("Host=h;AuthenticationType=Certificate").needsUser());

  ConnectionConfig c = configFrom(
      "Host=h;UnicodeTypes=0;DefaultStringColumnLength=42;UnboundedVarcharAsLongVarchar=1;"
      "TimestampWithTimeZoneMode=string;TimeWithTimeZoneMode=utc;IntervalMode=interval;UuidAsGuid=1;DecimalAsDouble=1");
  TypeMappingOptions o = c.typeMapping();
  CHECK(!o.unicode);
  CHECK_EQ(o.defaultStringLength, 42);
  CHECK(o.unboundedAsLong);
  CHECK(o.timestampTz == TimestampTzMode::String);
  CHECK(o.timeTz == TimeTzMode::Utc);
  CHECK(o.interval == IntervalMode::Interval);
  CHECK(o.uuidAsGuid);
  CHECK(o.decimalAsDouble);
}

// ---- composite values ------------------------------------------------------------------------------

TEST_CASE("config: SessionProperties, ExtraCredentials, Roles, ClientTags, ExtraHeaders") {
  ConnectionConfig c = configFrom(
      "Host=h;SessionProperties={query_max_run_time=1h; hive.max_split_size=64MB,join_distribution_type=BROADCAST};"
      "ExtraCredentials={foo=bar;baz=qux};Roles={system=ROLE{admin}};tpch=ALL;hive=analyst;mysql=none};"
      "ClientTags=etl, powerbi ,,tag3;ExtraHeaders={X-Custom:a,b;X-Other: value};TraceToken=tt");
  CHECK_EQ(c.sessionProperties.size(), size_t(3));
  CHECK_EQ(c.sessionProperties["query_max_run_time"], std::string("1h"));
  CHECK_EQ(c.sessionProperties["hive.max_split_size"], std::string("64MB"));
  CHECK_EQ(c.sessionProperties["join_distribution_type"], std::string("BROADCAST"));
  CHECK_EQ(c.extraCredentials.size(), size_t(2));
  CHECK_EQ(c.extraCredentials["foo"], std::string("bar"));
  CHECK_EQ(c.extraCredentials["baz"], std::string("qux"));
  CHECK_EQ(c.roles.size(), size_t(4));
  CHECK_EQ(c.roles["system"], std::string("ROLE{admin}"));
  CHECK_EQ(c.roles["tpch"], std::string("ALL"));
  CHECK_EQ(c.roles["hive"], std::string("ROLE{analyst}"));
  CHECK_EQ(c.roles["mysql"], std::string("NONE"));
  REQUIRE_EQ(c.clientTags.size(), size_t(3));
  CHECK_EQ(c.clientTags[0], std::string("etl"));
  CHECK_EQ(c.clientTags[1], std::string("powerbi"));
  CHECK_EQ(c.clientTags[2], std::string("tag3"));
  REQUIRE_EQ(c.extraHeaders.size(), size_t(2));
  CHECK_EQ(c.extraHeaders[0].first, std::string("X-Custom"));
  CHECK_EQ(c.extraHeaders[0].second, std::string("a,b"));
  CHECK_EQ(c.extraHeaders[1].first, std::string("X-Other"));
  CHECK_EQ(c.extraHeaders[1].second, std::string("value"));
  CHECK_EQ(c.traceToken, std::string("tt"));

  // A bare role applies to the system role.
  ConnectionConfig bare = configFrom("Host=h;Roles=admin");
  CHECK_EQ(bare.roles["system"], std::string("ROLE{admin}"));
  // Malformed entries are rejected.
  CHECK_THROWS_AS(configFrom("Host=h;SessionProperties=novalue"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;ExtraCredentials==x"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;ExtraHeaders=NoColon"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;ExtraHeaders={Bad Name: v}"), DriverError);
}

TEST_CASE("config: numeric ranges and enum values") {
  CHECK_THROWS_AS(configFrom("Host=h;Port=0"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;Port=70000"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;Port=abc"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;ConnectTimeout=-1"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;PrefetchPages=0"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;MaxParallelSegments=0"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;TargetResultSize=0"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;MinTLSVersion=1.1"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;QueryDataEncoding=json+snappy"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;TimestampWithTimeZoneMode=local-ish"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;IntervalMode=numeric"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;ExternalAuthRedirectHandler=popup"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;AnsiEncoding=latin1"), DriverError);

  ConnectionConfig c = configFrom(
      "Host=h;TargetResultSize=1000;MinTLSVersion=TLSv1.3;QueryDataEncoding=JSON+LZ4,json;LogLevel=trace;"
      "ExternalAuthRedirectHandler=print;AnsiEncoding=system;TimeWithTimeZoneMode=wallclock");
  CHECK_EQ(c.targetResultSizeMb, 128);  // clamped
  CHECK_EQ(c.minTlsVersion, std::string("1.3"));
  CHECK_EQ(c.queryDataEncoding, std::string("json+lz4,json"));
  CHECK_EQ(c.logLevel, 5);
  CHECK(c.externalAuthRedirectHandler == RedirectHandler::Print);
  CHECK(c.ansiEncoding == AnsiEncoding::System);
  CHECK(c.timeTzMode == TimeTzMode::WallClock);
  // Out-of-range log levels are clamped rather than rejected.
  CHECK_EQ(configFrom("Host=h;LogLevel=9").logLevel, 5);
  // Empty numeric/boolean entries (typical for DSNs written by setup dialogs) keep the default.
  c = configFrom("Host=h;Port=;SSL=;Compression=;LogLevel=");
  CHECK_EQ(c.port, 8080);
  CHECK(!c.sslExplicit);
  CHECK(c.compression);
}

TEST_CASE("config: unknown keys are kept, not rejected") {
  ConnectionConfig c = configFrom("Host=h;FooBar=1;Baz={x;y}");
  CHECK_EQ(c.unknown.size(), size_t(2));
  CHECK_EQ(c.unknown["foobar"], std::string("1"));
  CHECK_EQ(c.unknown["Baz"], std::string("x;y"));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;FooBar=1")), std::string(""));
}

// ---- validate rules --------------------------------------------------------------------------------

TEST_CASE("config: validate SQLSTATEs") {
  // Host required.
  CHECK_EQ(validateState(configFrom("UID=u")), std::string("HY000"));
  CHECK_EQ(validateState(configFrom("Host=   ;UID=u")), std::string("HY000"));
  // Plain connection needs a user.
  CHECK_EQ(validateState(configFrom("Host=h")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u")), std::string(""));
  // LDAP: needs UID+PWD and SSL (or AllowInsecurePassword).
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=LDAP;UID=u")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;PWD=p;SSL=0")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;PWD=p;SSL=0;AllowInsecurePassword=1")), std::string(""));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;PWD=p")), std::string(""));
  // JWT needs a token and a user.
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;AuthenticationType=JWT")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;AccessToken=t")), std::string(""));
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;AccessToken=t;SSL=0")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;AccessToken=t")), std::string("28000"));
  // Certificate needs the client certificate.
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=Certificate")), std::string("28000"));
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=Certificate;ClientCert=/c.pem")), std::string(""));
  // Kerberos / OAuth2 resolve the user server-side.
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=Kerberos")), std::string(""));
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=OAuth2")), std::string(""));
  CHECK_EQ(validateState(configFrom("Host=h;AuthenticationType=Kerberos;SSL=0")), std::string("28000"));
  // Inconsistent attribute values -> HY024.
  ConnectionConfig c = configFrom("Host=h;UID=u;HttpProxy=a:1;SocksProxy=b:2");
  CHECK_EQ(validateState(c), std::string("HY024"));
  c = configFrom("Host=h;UID=u");
  c.port = 99999;
  CHECK_EQ(validateState(c), std::string("HY024"));
  c = configFrom("Host=h;UID=u");
  c.minTlsVersion = "1.0";
  CHECK_EQ(validateState(c), std::string("HY024"));
  c = configFrom("Host=h;UID=u");
  c.targetResultSizeMb = 129;
  CHECK_EQ(validateState(c), std::string("HY024"));
  c = configFrom("Host=h;UID=u");
  c.queryDataEncoding = "json,zstd";
  CHECK_EQ(validateState(c), std::string("HY024"));
  // SSLVerification=None is allowed (only logged).
  CHECK_EQ(validateState(configFrom("Host=h;UID=u;PWD=p;SSLVerification=None")), std::string(""));
  // validate() throws a DriverError, never anything else.
  CHECK_THROWS_AS(configFrom("Host=h").validate(), DriverError);
}

// ---- serialisation -------------------------------------------------------------------------------

TEST_CASE("config: toConnectionString round trip with braces and secret masking") {
  ConnectionConfig c = configFrom(
      "DSN=my dsn;Host=h;Port=8443;UID=alice;PWD={p;w}}d};Catalog=tpch;SessionProperties={a=1;b=2};"
      "ClientTags=x,y;Roles=system=ROLE{admin};Weird=1");
  const std::string masked = c.toConnectionString(false);
  CHECK(masked.find("PWD=***") != std::string::npos);
  CHECK(masked.find("p;w") == std::string::npos);
  CHECK(masked.find("Weird=1") != std::string::npos);
  CHECK(masked.find("DSN=my dsn") != std::string::npos);
  CHECK(masked.find("AuthenticationType=LDAP") != std::string::npos);
  CHECK(masked.find("SSL=1") != std::string::npos);
  CHECK(masked.find("Port=8443") != std::string::npos);
  // Canonical order: DSN comes first, Host before Port before UID.
  CHECK(masked.find("DSN=") < masked.find("Host="));
  CHECK(masked.find("Host=") < masked.find("Port="));
  CHECK(masked.find("Port=") < masked.find("UID="));

  const std::string full = c.toConnectionString(true);
  CHECK(full.find("PWD={p;w}}d}") != std::string::npos);
  CHECK(full.find("SessionProperties={a=1;b=2}") != std::string::npos);
  CHECK(full.find("Roles={system=ROLE{admin}}}") != std::string::npos);

  // Parsing the serialised string reproduces the configuration.
  ConnectionConfig back = configFrom(full);
  CHECK_EQ(back.host, c.host);
  CHECK_EQ(back.port, c.port);
  CHECK_EQ(back.user, c.user);
  CHECK_EQ(back.password, std::string("p;w}d"));
  CHECK_EQ(back.catalog, c.catalog);
  CHECK(back.auth == AuthType::Ldap);
  CHECK(back.ssl);
  CHECK_EQ(back.sessionProperties["a"], std::string("1"));
  CHECK_EQ(back.sessionProperties["b"], std::string("2"));
  CHECK_EQ(back.roles["system"], std::string("ROLE{admin}"));
  REQUIRE_EQ(back.clientTags.size(), size_t(2));
  CHECK_EQ(back.clientTags[1], std::string("y"));
  CHECK_EQ(back.unknown["Weird"], std::string("1"));
  CHECK_EQ(back.dsn, std::string("my dsn"));
}

TEST_CASE("config: toConnectionString omits keys that were never set") {
  ConnectionConfig c = configFrom("Host=h;UID=u");
  const std::string s = c.toConnectionString(true);
  CHECK_EQ(s, std::string("Host=h;Port=8080;UID=u;SSL=0"));
  ConnectionConfig d = configFrom("Host=h;SSL=1;SSLVerification=None;AllowSelfSignedServerCert=1");
  const std::string t = d.toConnectionString(true);
  CHECK(t.find("SSLVerification=None") != std::string::npos);
  CHECK(t.find("AllowSelfSigned") == std::string::npos);  // folded into SSLVerification
}

TEST_CASE("config: baseUrl brackets IPv6 hosts") {
  CHECK_EQ(configFrom("Host=::1;Port=9000").baseUrl(), std::string("http://[::1]:9000"));
  CHECK_EQ(configFrom("Host=[fe80::1];SSL=1").baseUrl(), std::string("https://[fe80::1]:443"));
  CHECK_EQ(configFrom("Host=10.0.0.1").baseUrl(), std::string("http://10.0.0.1:8080"));
}

TEST_CASE("config: applyKeyValue reports the offending key") {
  ConnectionConfig c;
  std::string err;
  CHECK(!c.applyKeyValue("Port", "eighty", &err));
  CHECK(err.find("Port") != std::string::npos);
  CHECK(err.find("eighty") != std::string::npos);
  err.clear();
  CHECK(!c.applyKeyValue("MaxPrefetchBytes", "0", &err));
  CHECK(err.find("MaxPrefetchBytes") != std::string::npos);
  // Unknown keys succeed.
  CHECK(c.applyKeyValue("Nope", "1", &err));
  CHECK_EQ(c.unknown["nope"], std::string("1"));
  // Secrets keep surrounding whitespace, ordinary strings are trimmed.
  CHECK(c.applyKeyValue("PWD", " secret ", &err));
  CHECK_EQ(c.password, std::string(" secret "));
  CHECK(c.applyKeyValue("Catalog", " tpch ", &err));
  CHECK_EQ(c.catalog, std::string("tpch"));
}

// Review finding [48]: a malformed ExtraCredentials / ExtraHeaders value must not be echoed into the
// error text (it becomes the diagnostic message, the trace log and the application's error dialog).
TEST_CASE("config: malformed secret list values are not echoed in errors") {
  const std::string secret = "AKIAsuperSecretValue";
  {
    ConnectionConfig c;
    std::string err;
    CHECK(!c.applyKeyValue("ExtraCredentials", "s3.access=ok;s3.secret-key:" + secret, &err));
    CHECK(err.find("ExtraCredentials") != std::string::npos);
    CHECK(err.find("entry 2") != std::string::npos);
    CHECK(err.find(secret) == std::string::npos);
  }
  {
    ConnectionConfig c;
    std::string err;
    CHECK(!c.applyKeyValue("ExtraHeaders", "Authorization Bearer " + secret, &err));
    CHECK(err.find("ExtraHeaders") != std::string::npos);
    CHECK(err.find(secret) == std::string::npos);
    err.clear();
    CHECK(!c.applyKeyValue("ExtraHeaders", "X-Ok:1;Bad Name:" + secret, &err));
    CHECK(err.find("entry 2") != std::string::npos);
    CHECK(err.find(secret) == std::string::npos);
  }
  // The DriverError thrown by applyAll carries the same redacted text.
  try {
    configFrom("Host=h;ExtraCredentials={k:" + secret + "}");
    CHECK(false);
  } catch (const DriverError& e) {
    CHECK(std::string(e.what()).find(secret) == std::string::npos);
    CHECK(e.message().find(secret) == std::string::npos);
  }
  // Non-secret list keys still show the offending value (it helps fixing a typo).
  ConnectionConfig plain;
  std::string err;
  CHECK(!plain.applyKeyValue("SessionProperties", "novalue", &err));
  CHECK(err.find("novalue") != std::string::npos);
}

// Review finding [49]: a DSN name that was dropped (it did not exist and DRIVER= was given) must not
// be emitted, or a driver manager would look the missing DSN up again on reconnect.
TEST_CASE("config: toConnectionString omits a cleared DSN") {
  ConnectionConfig c = configFrom("DRIVER={Trino ODBC Driver};DSN=placeholder;Host=h;UID=u");
  CHECK(c.toConnectionString(true).find("DSN=placeholder") != std::string::npos);
  c.dsn.clear();
  const std::string s = c.toConnectionString(true);
  CHECK(s.find("DSN=") == std::string::npos);
  CHECK(s.rfind("Driver=", 0) == 0);
  CHECK(s.find("Host=h") != std::string::npos);
}

TEST_CASE("config: IdleFetchTimeout (AbandonedQueryTimeout)") {
  // Off by default.
  CHECK_EQ(configFrom("Host=h").idleFetchTimeoutSec, 0);
  CHECK_EQ(configFrom("Host=h;IdleFetchTimeout=600").idleFetchTimeoutSec, 600);
  CHECK_EQ(configFrom("Host=h;idlefetchtimeout=2").idleFetchTimeoutSec, 2);
  CHECK_EQ(configFrom("Host=h;AbandonedQueryTimeout=900").idleFetchTimeoutSec, 900);
  CHECK_EQ(configFrom("Host=h;IdleFetchTimeout=0").idleFetchTimeoutSec, 0);
  CHECK_EQ(ConnectionConfig::canonicalKey("AbandonedQueryTimeout"), std::string("IdleFetchTimeout"));
  CHECK(!ConnectionConfig::isSecretKey("IdleFetchTimeout"));
  CHECK_THROWS_AS(configFrom("Host=h;IdleFetchTimeout=-1"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;IdleFetchTimeout=soon"), DriverError);
  CHECK_THROWS_AS(configFrom("Host=h;IdleFetchTimeout=999999999"), DriverError);
  // Round trip through the canonical connection string.
  const ConnectionConfig c = configFrom("Host=h;AbandonedQueryTimeout=600");
  const std::string out = c.toConnectionString(false);
  CHECK(out.find("IdleFetchTimeout=600") != std::string::npos);
  CHECK_EQ(configFrom(out).idleFetchTimeoutSec, 600);
}

TEST_CASE("config: TimeZone defaults to Europe/Istanbul, 'system' selects the OS zone") {
  CHECK_EQ(std::string(kDefaultSessionTimeZone), std::string("Europe/Istanbul"));
  ConnectionConfig c = configFrom("Host=h");
  CHECK_EQ(c.timeZone, std::string("Europe/Istanbul"));
  CHECK_EQ(resolveSessionTimeZone(c.timeZone), std::string("Europe/Istanbul"));
  // The filled-in default is not an explicit setting: it is not echoed in the connection string.
  CHECK(c.toConnectionString(false).find("TimeZone") == std::string::npos);
  // An empty value (as setup dialogs write it) keeps the default.
  CHECK_EQ(configFrom("Host=h;TimeZone=").timeZone, std::string("Europe/Istanbul"));
  CHECK_EQ(configFrom("Host=h;TimeZone=  ").timeZone, std::string("Europe/Istanbul"));

  ConnectionConfig sys = configFrom("Host=h;TimeZone=SYSTEM");
  CHECK_EQ(sys.timeZone, std::string("system"));
  CHECK_EQ(resolveSessionTimeZone(sys.timeZone), TimeZoneDb::systemZoneName());
  CHECK(sys.toConnectionString(false).find("TimeZone=system") != std::string::npos);

  ConnectionConfig ny = configFrom("Host=h;TimeZone= America/New_York ");
  CHECK_EQ(ny.timeZone, std::string("America/New_York"));
  CHECK_EQ(resolveSessionTimeZone(ny.timeZone), std::string("America/New_York"));
  CHECK_EQ(configFrom("Host=h;TimeZone=+05:30").timeZone, std::string("+05:30"));

  // Before finalize() the raw value is empty; the resolver still yields the default.
  CHECK_EQ(resolveSessionTimeZone(""), std::string("Europe/Istanbul"));
  CHECK_EQ(resolveSessionTimeZone("System"), TimeZoneDb::systemZoneName());
}

TEST_CASE("config: timestamp/time with time zone modes, session value and aliases") {
  CHECK(ConnectionConfig().timestampTzMode == TimestampTzMode::Session);
  CHECK(TypeMappingOptions().timestampTz == TimestampTzMode::Session);
  CHECK(configFrom("Host=h").typeMapping().timestampTz == TimestampTzMode::Session);
  for (const char* word : {"session", "SESSION", "local", "sessiontz", "SessionTZ", "session_tz", "session-tz"}) {
    CHECK(configFrom(std::string("Host=h;TimestampWithTimeZoneMode=") + word).timestampTzMode == TimestampTzMode::Session);
    CHECK(configFrom(std::string("Host=h;TimeWithTimeZoneMode=") + word).timeTzMode == TimeTzMode::Session);
  }
  CHECK(configFrom("Host=h;TimestampWithTimeZoneMode=utc").timestampTzMode == TimestampTzMode::Utc);
  CHECK(configFrom("Host=h;TimestampTZMode=wallclock").timestampTzMode == TimestampTzMode::WallClock);
  CHECK(configFrom("Host=h;TimestampWithTimeZoneMode=wall-clock").timestampTzMode == TimestampTzMode::WallClock);
  CHECK(configFrom("Host=h;TimestampWithTimeZoneMode=text").timestampTzMode == TimestampTzMode::String);
  CHECK(configFrom("Host=h;TimestampWithTimeZoneMode=").timestampTzMode == TimestampTzMode::Session);
  CHECK(configFrom("Host=h;TimeWithTimeZoneMode=").timeTzMode == TimeTzMode::String);  // time tz default unchanged
  CHECK_THROWS_AS(configFrom("Host=h;TimestampWithTimeZoneMode=istanbul"), DriverError);
  // Canonical names in the generated connection string.
  const std::string out = configFrom("Host=h;TimestampTZMode=local;TimeWithTimeZoneMode=sessiontz").toConnectionString(false);
  CHECK(out.find("TimestampWithTimeZoneMode=session") != std::string::npos);
  CHECK(out.find("TimeWithTimeZoneMode=session") != std::string::npos);
}
