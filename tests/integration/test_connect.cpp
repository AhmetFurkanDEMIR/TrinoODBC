// Connection tests: SQLDriverConnect (A/W), out string, error paths, authentication against the
// Basic/Bearer/TLS mock instances, environment version rule and reconnect behaviour.
#include "integration/odbc_loader.h"

#include <algorithm>
#include <cctype>
#include <string>

using namespace harness;

// ODBC 3.8 names missing from older driver-manager headers (iODBC ships ODBC 3.52 headers).
#ifndef SQL_ATTR_RESET_CONNECTION
#define SQL_ATTR_RESET_CONNECTION 116
#endif
#ifndef SQL_RESET_CONNECTION_YES
#define SQL_RESET_CONNECTION_YES 1UL
#endif

namespace {

/// Case-insensitive "haystack contains needle".
bool icontains(const std::string& haystack, const std::string& needle) {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string standaloneConnStr(const std::string& hostPort, const std::string& extra) {
  std::string s = "Driver=" + driverPath() + ";" + hostPort + ";UID=test;LogLevel=" + std::to_string(ctx().logLevel);
  if (!ctx().logPath.empty()) s += ";LogPath=" + ctx().logPath;
  if (!extra.empty()) s += ";" + extra;
  return s;
}

}  // namespace

TEST_CASE("connect/exports_present") {
  const std::vector<std::string>& missing = missingExports();
  std::string list;
  for (const std::string& m : missing) list += (list.empty() ? "" : ", ") + m;
  CHECK(missing.empty());
  if (!missing.empty()) info("missing exports: %s", list.c_str());
}

TEST_CASE("connect/driver_connect_w") {
  Env env;
  Dbc dbc(env);
  std::string out;
  ODBC_REQUIRE(dbc.connect(ctx().connStr, &out), SQL_HANDLE_DBC, dbc.h);
  CHECK(!out.empty());
  CHECK(icontains(out, "Host="));
  CHECK(icontains(out, "Port="));
  CHECK_EQ(getConnectAttrInt(dbc.h, SQL_ATTR_CONNECTION_DEAD), static_cast<SQLULEN>(SQL_CD_FALSE));
  // The out string must reconnect on its own.
  Dbc again(env);
  ODBC_CHECK(again.connect(out), SQL_HANDLE_DBC, again.h);
  ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
}

TEST_CASE("connect/driver_connect_a") {
  Env env;
  Dbc dbc(env);
  std::string out;
  ODBC_REQUIRE(dbc.connectA(ctx().connStr, &out), SQL_HANDLE_DBC, dbc.h);
  CHECK(icontains(out, "Host="));
  Stmt stmt(dbc);
  ODBC_CHECK(execDirectA(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getInt64(stmt.h, 1).value_or(-1), static_cast<int64_t>(1));
}

TEST_CASE("connect/out_string_keeps_password") {
  HARNESS_MOCK_ONLY();
  // ODBC requires the returned connection string to be usable for reconnecting, so the
  // password the application supplied must come back unmasked (toConnectionString(true)).
  Env env;
  Dbc dbc(env);
  std::string out;
  ODBC_REQUIRE(dbc.connect(ctx().connStrWith("AuthenticationType=None;PWD=s3cretValue"), &out), SQL_HANDLE_DBC, dbc.h);
  CHECK(icontains(out, "PWD=s3cretValue"));
  CHECK(!icontains(out, "PWD=***"));
}

TEST_CASE("connect/out_string_buffer_too_small_01004") {
  Env env;
  Dbc dbc(env);
  WStr in(ctx().connStr);
  SQLWCHAR out[8] = {0};
  SQLSMALLINT outLen = 0;
  const SQLRETURN rc = Odbc.SQLDriverConnectW(dbc.h, nullptr, in.ptr(), in.len(), out, 8, &outLen, SQL_DRIVER_NOPROMPT);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_DBC, dbc.h, "01004");
  CHECK(outLen > 8);        // full length reported
  CHECK(out[7] == 0);       // always NUL-terminated
  if (SQL_SUCCEEDED(rc)) Odbc.SQLDisconnect(dbc.h);
}

TEST_CASE("connect/already_connected_08002") {
  Session s;
  WStr in(ctx().connStr);
  SQLSMALLINT outLen = 0;
  const SQLRETURN rc = Odbc.SQLDriverConnectW(s.h(), nullptr, in.ptr(), in.len(), nullptr, 0, &outLen, SQL_DRIVER_NOPROMPT);
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, s.h(), "08002");
}

TEST_CASE("connect/wrong_host_08001") {
  const int closedPort = findFreePort();  // nobody listens here
  REQUIRE(closedPort > 0);
  Env env;
  Dbc dbc(env);
  const SQLRETURN rc = dbc.connect(standaloneConnStr("Host=127.0.0.1;Port=" + std::to_string(closedPort), "ConnectTimeout=5"));
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, dbc.h, "08001");
  const std::vector<DiagRec> d = diag(SQL_HANDLE_DBC, dbc.h);
  REQUIRE(!d.empty());
  CHECK(d.front().message.rfind("[Trino][ODBC Driver]", 0) == 0);
}

TEST_CASE("connect/missing_host_is_error") {
  Env env;
  Dbc dbc(env);
  const SQLRETURN rc = dbc.connect("Driver=" + driverPath() + ";UID=test");
  CHECK(rc == SQL_ERROR);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_DBC, dbc.h);
  REQUIRE(!d.empty());
  CHECK(icontains(d.front().message, "Host"));
}

TEST_CASE("connect/env_version_required_before_dbc_hy010") {
  SQLHENV env = SQL_NULL_HENV;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_HANDLE_ENV, env);
  SQLHDBC dbc = SQL_NULL_HDBC;
  const SQLRETURN rc = Odbc.SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_ENV, env, "HY010");
  if (SQL_SUCCEEDED(rc)) Odbc.SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  ODBC_CHECK(Odbc.SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, attrVal(SQL_OV_ODBC3), 0), SQL_HANDLE_ENV, env);
  ODBC_CHECK(Odbc.SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc), SQL_HANDLE_ENV, env);
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DBC, dbc), SQL_HANDLE_DBC, dbc);
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_ENV, env), SQL_HANDLE_ENV, env);
}

TEST_CASE("connect/login_timeout_before_connect") {
  Env env;
  Dbc dbc(env);
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(dbc.h, SQL_ATTR_LOGIN_TIMEOUT, attrVal(15), 0), SQL_HANDLE_DBC, dbc.h);
  CHECK_EQ(getConnectAttrInt(dbc.h, SQL_ATTR_LOGIN_TIMEOUT), static_cast<SQLULEN>(15));
  ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
}

TEST_CASE("connect/disconnect_and_reconnect_same_handle") {
  Env env;
  Dbc dbc(env);
  ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
  {
    Stmt stmt(dbc);
    ODBC_CHECK(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
    // Disconnect with an open cursor must succeed (the driver closes it).
  }
  ODBC_CHECK(dbc.disconnect(), SQL_HANDLE_DBC, dbc.h);
  CHECK_EQ(getConnectAttrInt(dbc.h, SQL_ATTR_CONNECTION_DEAD), static_cast<SQLULEN>(SQL_CD_TRUE));
  // Statements need a connection: 08003.
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  ODBC_CHECK_ERROR(Odbc.SQLAllocHandle(SQL_HANDLE_STMT, dbc.h, &stmt), SQL_HANDLE_DBC, dbc.h, "08003");
  ODBC_REQUIRE(dbc.connect(ctx().connStr), SQL_HANDLE_DBC, dbc.h);
  Stmt again(dbc);
  ODBC_CHECK(execDirect(again.h, "SELECT 1"), SQL_HANDLE_STMT, again.h);
}

TEST_CASE("connect/browse_connect_need_data") {
  Env env;
  Dbc dbc(env);
  WStr in("Driver=" + driverPath());
  std::vector<SQLWCHAR> out(1024);
  SQLSMALLINT outLen = 0;
  const SQLRETURN rc = Odbc.SQLBrowseConnectW(dbc.h, in.ptr(), in.len(), out.data(), static_cast<SQLSMALLINT>(out.size()), &outLen);
  CHECK(rc == SQL_NEED_DATA);
  const std::string text = fromWide(out.data(), std::min<SQLLEN>(outLen, 1023));
  CHECK(icontains(text, "Host"));
  CHECK(icontains(text, "UID"));
}

TEST_CASE("connect/validate_connection_off_sends_nothing") {
  HARNESS_MOCK_ONLY();
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  Env env;
  Dbc dbc(env);
  ODBC_REQUIRE(dbc.connect(ctx().connStrWith("ValidateConnection=0")), SQL_HANDLE_DBC, dbc.h);
  const std::optional<MockStats> after = mockStats();
  REQUIRE(after.has_value());
  CHECK_EQ(after->queries, before->queries);
  // The connection is still usable.
  Stmt stmt(dbc);
  ODBC_CHECK(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
}

TEST_CASE("connect/ldap_over_http_requires_ssl_28000") {
  HARNESS_MOCK_ONLY();
  const std::optional<MockStats> before = mockStats();
  REQUIRE(before.has_value());
  Env env;
  Dbc dbc(env);
  const SQLRETURN rc = dbc.connect(ctx().mockConnStr(ctx().port, "SSL=0;AuthenticationType=LDAP;UID=alice;PWD=secret"));
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, dbc.h, "28000");
  // Rejected client-side: nothing reached the server.
  const std::optional<MockStats> after = mockStats();
  REQUIRE(after.has_value());
  CHECK_EQ(after->requests, before->requests);
}

TEST_CASE("connect/basic_auth_correct_password") {
  HARNESS_MOCK_ONLY();
  if (ctx().basicAuthPort == 0) {
    skip("basic-auth mock instance not running");
    return;
  }
  Env env;
  Dbc dbc(env);
  ODBC_REQUIRE(dbc.connect(ctx().mockConnStr(ctx().basicAuthPort,
                                             "AuthenticationType=LDAP;UID=alice;PWD=secret;AllowInsecurePassword=1")),
               SQL_HANDLE_DBC, dbc.h);
  Stmt stmt(dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_user"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("alice"));
  CHECK_EQ(getInfoString(dbc.h, SQL_USER_NAME), std::string("alice"));
}

TEST_CASE("connect/basic_auth_wrong_password_28000") {
  HARNESS_MOCK_ONLY();
  if (ctx().basicAuthPort == 0) {
    skip("basic-auth mock instance not running");
    return;
  }
  Env env;
  Dbc dbc(env);
  const SQLRETURN rc = dbc.connect(ctx().mockConnStr(ctx().basicAuthPort,
                                                     "AuthenticationType=LDAP;UID=alice;PWD=wrong;AllowInsecurePassword=1"));
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, dbc.h, "28000");
  // Without credentials at all the server answers 401 as well.
  Dbc none(env);
  const SQLRETURN rc2 = none.connect(ctx().mockConnStr(ctx().basicAuthPort, ""));
  ODBC_CHECK_ERROR(rc2, SQL_HANDLE_DBC, none.h, "28000");
}

TEST_CASE("connect/basic_auth_alias_keys") {
  HARNESS_MOCK_ONLY();
  if (ctx().basicAuthPort == 0) {
    skip("basic-auth mock instance not running");
    return;
  }
  // Simba/JDBC-style aliases: AuthMech=Password, User/Password keys.
  Env env;
  Dbc dbc(env);
  const std::string cs = "Driver=" + driverPath() + ";Server=127.0.0.1;Port=" + std::to_string(ctx().basicAuthPort) +
                         ";AuthMech=Password;User=alice;Password=secret;AllowInsecurePassword=1;Database=mock";
  ODBC_REQUIRE(dbc.connect(cs), SQL_HANDLE_DBC, dbc.h);
  CHECK_EQ(getInfoString(dbc.h, SQL_DATABASE_NAME), std::string("mock"));
}

TEST_CASE("connect/bearer_token") {
  HARNESS_MOCK_ONLY();
  if (ctx().bearerPort == 0) {
    skip("bearer mock instance not running");
    return;
  }
  Env env;
  Dbc ok(env);
  ODBC_CHECK(ok.connect(ctx().mockConnStr(ctx().bearerPort, "AuthenticationType=JWT;AccessToken=" + ctx().bearerToken +
                                                                ";AllowInsecurePassword=1")),
             SQL_HANDLE_DBC, ok.h);
  Dbc bad(env);
  const SQLRETURN rc = bad.connect(ctx().mockConnStr(ctx().bearerPort, "AuthenticationType=JWT;AccessToken=nope;AllowInsecurePassword=1"));
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, bad.h, "28000");
  // Giving a token without an auth type selects JWT automatically.
  Dbc implicit(env);
  ODBC_CHECK(implicit.connect(ctx().mockConnStr(ctx().bearerPort, "AccessToken=" + ctx().bearerToken + ";AllowInsecurePassword=1")),
             SQL_HANDLE_DBC, implicit.h);
}

TEST_CASE("connect/tls_verification_none") {
  HARNESS_MOCK_ONLY();
  if (ctx().tlsPort == 0) {
    skip("TLS mock instance not running");
    return;
  }
  Env env;
  Dbc dbc(env);
  ODBC_REQUIRE(dbc.connect(ctx().mockConnStr(ctx().tlsPort, "SSL=1;SSLVerification=None")), SQL_HANDLE_DBC, dbc.h);
  Stmt stmt(dbc);
  ODBC_CHECK(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  // Simba alias for the same thing.
  Dbc alias(env);
  ODBC_CHECK(alias.connect(ctx().mockConnStr(ctx().tlsPort, "SSL=1;AllowSelfSignedServerCert=1")), SQL_HANDLE_DBC, alias.h);
}

TEST_CASE("connect/tls_untrusted_cert_rejected") {
  HARNESS_MOCK_ONLY();
  if (ctx().tlsPort == 0) {
    skip("TLS mock instance not running");
    return;
  }
  Env env;
  Dbc dbc(env);
  const SQLRETURN rc = dbc.connect(ctx().mockConnStr(ctx().tlsPort, "SSL=1;SSLVerification=Full;UseSystemTrustStore=0"));
  CHECK(rc == SQL_ERROR);
  CHECK(hasState(SQL_HANDLE_DBC, dbc.h, "08001"));
  // Plain http against the TLS port must fail cleanly too (no hang, no crash).
  Dbc plain(env);
  const SQLRETURN rc2 = plain.connect(ctx().mockConnStr(ctx().tlsPort, "SSL=0;ConnectTimeout=5;RequestTimeout=5"));
  CHECK(rc2 == SQL_ERROR);
}

TEST_CASE("connect/tls_trusted_certs_full_verification") {
  HARNESS_MOCK_ONLY();
  if (ctx().tlsPort == 0) {
    skip("TLS mock instance not running");
    return;
  }
  Env env;
  Dbc dbc(env);
  // The certificate names localhost, so the hostname check of Full verification must pass.
  const SQLRETURN rc = dbc.connect(standaloneConnStr("Host=localhost;Port=" + std::to_string(ctx().tlsPort),
                                                     "SSL=1;SSLVerification=Full;TrustedCerts=" + ctx().tlsCertFile +
                                                         ";UseSystemTrustStore=0;Catalog=mock;Schema=demo"));
  ODBC_CHECK(rc, SQL_HANDLE_DBC, dbc.h);
  // CA mode with the wrong host name in the URL still verifies the chain only.
  Dbc ca(env);
  ODBC_CHECK(ca.connect(ctx().mockConnStr(ctx().tlsPort, "SSL=1;SSLVerification=CA;TrustedCerts=" + ctx().tlsCertFile +
                                                             ";UseSystemTrustStore=0")),
             SQL_HANDLE_DBC, ca.h);
}

TEST_CASE("connect/ssl_implied_by_port_443_is_not_forced_when_explicit") {
  HARNESS_MOCK_ONLY();
  // SSL=0 given explicitly must win over the port-443 heuristic: the connect fails on the
  // closed port with 08001 rather than with a TLS error, proving http was attempted.
  Env env;
  Dbc dbc(env);
  const int closedPort = findFreePort();
  const SQLRETURN rc = dbc.connect(standaloneConnStr("Host=127.0.0.1;Port=" + std::to_string(closedPort), "SSL=0;ConnectTimeout=3"));
  ODBC_CHECK_ERROR(rc, SQL_HANDLE_DBC, dbc.h, "08001");
}

TEST_CASE("connect/unknown_keys_are_tolerated") {
  Env env;
  Dbc dbc(env);
  ODBC_CHECK(dbc.connect(ctx().connStrWith("SomeUnknownKey=1;AnotherOne={a;b}")), SQL_HANDLE_DBC, dbc.h);
}

TEST_CASE("connect/sqlconnect_without_dsn_im002") {
  Env env;
  Dbc dbc(env);
  WStr dsn("trino_odbc_no_such_dsn_xyz");
  WStr uid("u");
  WStr pwd("p");
  const SQLRETURN rc = Odbc.SQLConnectW(dbc.h, dsn.ptr(), dsn.len(), uid.ptr(), uid.len(), pwd.ptr(), pwd.len());
  CHECK(rc == SQL_ERROR);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_DBC, dbc.h);
  CHECK(hasState(d, "IM002") || hasState(d, "HY000") || hasState(d, "28000"));
}

// Review finding [20]: SQL_ATTR_RESET_CONNECTION (set by an ODBC 3.8 driver manager before pooling)
// must restore the session and the connection attributes, and close the statements' cursors.
TEST_CASE("connect/reset_connection_restores_session_defaults") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "USE tpch.tiny"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(execDirect(stmt.h, "SET SESSION query_max_run_time = '10s'"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_schema"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("tiny"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_METADATA_ID, attrVal(SQL_TRUE), 0), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_ACCESS_MODE, attrVal(SQL_MODE_READ_ONLY), 0), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_TXN_ISOLATION, attrVal(SQL_TXN_SERIALIZABLE), 0), SQL_HANDLE_DBC, s.h());
  // Leave a cursor open: the next pool user must not inherit it.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);

  ODBC_REQUIRE(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_RESET_CONNECTION, attrVal(SQL_RESET_CONNECTION_YES),
                                       SQL_IS_UINTEGER),
               SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_METADATA_ID), static_cast<SQLULEN>(SQL_FALSE));
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_ACCESS_MODE), static_cast<SQLULEN>(SQL_MODE_READ_WRITE));
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_TXN_ISOLATION), static_cast<SQLULEN>(SQL_TXN_READ_UNCOMMITTED));
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_AUTOCOMMIT), static_cast<SQLULEN>(SQL_AUTOCOMMIT_ON));
  char catalog[64] = {0};
  SQLINTEGER catalogLen = 0;
  ODBC_CHECK(Odbc.SQLGetConnectAttr(s.h(), SQL_ATTR_CURRENT_CATALOG, catalog, sizeof(catalog), &catalogLen),
             SQL_HANDLE_DBC, s.h());
  CHECK_EQ(std::string(catalog), std::string("mock"));
  // The cursor was closed by the reset.
  CHECK(Odbc.SQLFetch(stmt.h) == SQL_ERROR);
  // The server sees the configured catalog / schema again (X-Trino-Catalog / X-Trino-Schema).
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_schema"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("demo"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT current_catalog"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getString(stmt.h, 1), std::string("mock"));
  // Only SQL_RESET_CONNECTION_YES is a valid value.
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_RESET_CONNECTION, attrVal(7), SQL_IS_UINTEGER), SQL_HANDLE_DBC,
                   s.h(), "HY024");
}

// Review finding [22]: a null WindowHandle means "no dialog boxes": every completion mode behaves like
// SQL_DRIVER_NOPROMPT (on Windows the driver used to open an unowned modal dialog and hang).
TEST_CASE("connect/null_window_handle_never_prompts") {
  const SQLUSMALLINT modes[] = {SQL_DRIVER_PROMPT, SQL_DRIVER_COMPLETE, SQL_DRIVER_COMPLETE_REQUIRED};
  for (const SQLUSMALLINT mode : modes) {
    Env env;
    Dbc dbc(env);
    WStr in(ctx().connStr);
    const SQLRETURN rc = Odbc.SQLDriverConnectW(dbc.h, nullptr, in.ptr(), in.len(), nullptr, 0, nullptr, mode);
    ODBC_CHECK(rc, SQL_HANDLE_DBC, dbc.h);
    if (SQL_SUCCEEDED(rc)) Odbc.SQLDisconnect(dbc.h);
  }
  // Missing values with COMPLETE and no window: an error, never a (blocking) dialog.
  Env env;
  Dbc dbc(env);
  WStr in("Driver=" + driverPath() + ";UID=test");
  const SQLRETURN rc = Odbc.SQLDriverConnectW(dbc.h, nullptr, in.ptr(), in.len(), nullptr, 0, nullptr, SQL_DRIVER_COMPLETE);
  CHECK(rc == SQL_ERROR);
  if (SQL_SUCCEEDED(rc)) Odbc.SQLDisconnect(dbc.h);
}

// Review finding [49]: DRIVER=...;DSN=<missing> connects on the inline values; the out string must
// not name the missing DSN (a driver manager would look it up first and fail with IM002).
TEST_CASE("connect/out_string_drops_missing_dsn") {
  HARNESS_MOCK_ONLY();  // needs the Driver= key the mock connection strings carry
  Env env;
  Dbc dbc(env);
  std::string out;
  ODBC_REQUIRE(dbc.connect(ctx().connStrWith("DSN=trino_odbc_placeholder_dsn_xyz"), &out), SQL_HANDLE_DBC, dbc.h);
  CHECK(!icontains(out, "DSN="));
  CHECK(!icontains(out, "trino_odbc_placeholder_dsn_xyz"));
  CHECK(icontains(out, "Driver="));
  SQLWCHAR name[128] = {0};
  SQLSMALLINT nameLen = 0;
  ODBC_CHECK(Odbc.SQLGetInfoW(dbc.h, SQL_DATA_SOURCE_NAME, name, sizeof(name), &nameLen), SQL_HANDLE_DBC, dbc.h);
  CHECK_EQ(fromWide(name, nameLen / static_cast<SQLSMALLINT>(sizeof(SQLWCHAR))), std::string());
  Dbc again(env);
  ODBC_CHECK(again.connect(out), SQL_HANDLE_DBC, again.h);
}

// Review finding [55]: optional browse keywords carry the '*' prefix; PWD is only offered while the
// authentication type is still implicit and required only for LDAP.
TEST_CASE("connect/browse_connect_marks_optional_keywords") {
  auto browse = [](const std::string& inText, std::string* outText) {
    Env env;
    Dbc dbc(env);
    WStr in(inText);
    std::vector<SQLWCHAR> out(1024);
    SQLSMALLINT outLen = 0;
    const SQLRETURN rc =
        Odbc.SQLBrowseConnectW(dbc.h, in.ptr(), in.len(), out.data(), static_cast<SQLSMALLINT>(out.size()), &outLen);
    *outText = fromWide(out.data(), std::min<SQLLEN>(outLen, 1023));
    return rc;
  };
  std::string text;
  CHECK(browse("Driver=" + driverPath() + ";Host=127.0.0.1", &text) == SQL_NEED_DATA);
  const std::string wrapped = ";" + text;
  CHECK(wrapped.find(";UID:") != std::string::npos);         // required for AuthenticationType=None
  CHECK(wrapped.find(";*PWD:") != std::string::npos);        // optional: would select LDAP
  CHECK(wrapped.find(";PWD:") == std::string::npos);
  CHECK(wrapped.find(";*Catalog:") != std::string::npos);
  CHECK(wrapped.find(";*Schema:") != std::string::npos);
  CHECK(wrapped.find(";*SSL:") != std::string::npos);
  CHECK(wrapped.find(";Catalog:") == std::string::npos);
  CHECK(wrapped.find(";Schema:") == std::string::npos);
  CHECK(wrapped.find(";SSL:") == std::string::npos);
  CHECK(wrapped.find("Host:") == std::string::npos);         // already supplied
  // An explicitly password-less mode does not list PWD at all.
  CHECK(browse("Driver=" + driverPath() + ";Host=127.0.0.1;AuthenticationType=None", &text) == SQL_NEED_DATA);
  CHECK(!icontains(text, "PWD"));
  // LDAP needs the password: unprefixed.
  CHECK(browse("Driver=" + driverPath() + ";Host=127.0.0.1;AuthenticationType=LDAP;UID=u", &text) == SQL_NEED_DATA);
  CHECK((";" + text).find(";PWD:") != std::string::npos);
}

// Review finding [23]: SQLTransact goes through the driver's own transaction code (never the exported
// SQLEndTran, which may bind to the driver manager on Linux) for both the connection and env forms.
TEST_CASE("connect/sqltransact_env_and_dbc_forms") {
  Session s;
  ODBC_CHECK(Odbc.SQLTransact(s.env.h, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLTransact(SQL_NULL_HENV, s.h(), SQL_ROLLBACK), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLTransact(s.env.h, SQL_NULL_HDBC, SQL_COMMIT), SQL_HANDLE_ENV, s.env.h);
  ODBC_CHECK_ERROR(Odbc.SQLTransact(s.env.h, s.h(), 42), SQL_HANDLE_DBC, s.h(), "HY012");
  CHECK(Odbc.SQLTransact(SQL_NULL_HENV, SQL_NULL_HDBC, SQL_COMMIT) == SQL_INVALID_HANDLE);
}
