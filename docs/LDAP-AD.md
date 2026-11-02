# LDAP / Active Directory authentication

Trino authenticates users with a password by asking an LDAP server (for most companies: Active
Directory) to bind with the supplied credentials. Over ODBC this is plain HTTP Basic
authentication on top of TLS: the driver sends `Authorization: Basic base64(UID:PWD)` with every
request to the coordinator (`POST /v1/statement`, `GET nextUri`, `DELETE`); spooled-segment
downloads carry only the headers the server attached to the segment. Two things must therefore be
configured: the **Trino server** (HTTPS + password authenticator) and the **driver**
(`AuthenticationType=LDAP`).

> **What has been verified.** The driver side (Basic authentication over HTTPS, the `28000`
> errors below) was tested against a real Trino 483 with the file-based password authenticator,
> which Trino treats exactly like the LDAP authenticator on the HTTP side. The Active Directory
> server configuration in section 1 follows the Trino documentation but was not run against a real
> AD in this project.

## 1. Trino server configuration

### 1.1 HTTPS (`etc/config.properties`)

Trino refuses password authentication over plain HTTP, so the coordinator needs an HTTPS
listener. Add to `etc/config.properties`:

```properties
http-server.authentication.type=PASSWORD

http-server.https.enabled=true
http-server.https.port=8443
http-server.https.keystore.path=/etc/trino/keystore.p12
http-server.https.keystore.key=changeit

# Required as soon as authentication is enabled on a multi-node cluster
internal-communication.shared-secret=<long random string>
```

The keystore may be a PKCS#12 file (`.p12`) or a PEM file containing the private key and the
certificate chain. It must be issued for the host name users connect to
(`trino.corp.example.com`) as a Subject Alternative Name; otherwise every client needs
`SSLVerification=CA`.

Keep the plain HTTP port for internal communication only, or disable it with
`http-server.http.enabled=false` once the workers talk HTTPS too.

### 1.2 Password authenticator (`etc/password-authenticator.properties`)

Create `etc/password-authenticator.properties` on the coordinator. The following is a complete
configuration for Active Directory:

```properties
password-authenticator.name=ldap

# LDAPS to a domain controller (or the AD load-balanced name). Port 636 = LDAPS, 3269 = Global
# Catalog over TLS (use 3269 when users live in several domains of the forest).
ldap.url=ldaps://dc.corp.example.com:636

# How the user name typed by the user becomes the DN that Trino binds with.
# ${USER} is replaced by the user name. Several patterns are separated by ':' and tried in order.
#   jdoe@corp.example.com          -> UPN bind (works for every AD user, no OU needed)
#   CN=jdoe,OU=Users,DC=corp,...   -> classic DN bind (requires CN == login name)
ldap.user-bind-pattern=${USER}@corp.example.com:CN=${USER},OU=Users,DC=corp,DC=example,DC=com

# Optional: only members of a group may log in. The filter is evaluated after the bind succeeds,
# searching below ldap.user-base-dn. sAMAccountName is the classic AD login name.
ldap.user-base-dn=OU=Users,DC=corp,DC=example,DC=com
ldap.group-auth-pattern=(&(objectClass=person)(sAMAccountName=${USER})(memberOf=CN=TrinoUsers,OU=Groups,DC=corp,DC=example,DC=com))

# Service account used for the group search (recommended: AD usually does not let a user search
# with their own credentials outside their OU). If unset, the user's own bind is used.
ldap.bind-dn=CN=svc-trino,OU=Service Accounts,DC=corp,DC=example,DC=com
ldap.bind-password=<service account password>

# TLS trust for the LDAPS connection: the CA that issued the domain controller certificate.
# A Java truststore (JKS/PKCS#12) or a PEM file.
ldap.ssl.truststore.path=/etc/trino/ad-root-ca.pem
# ldap.ssl.truststore.password=changeit

# Plain ldap:// (no TLS) is refused unless this is true. Keep it false.
ldap.allow-insecure=false

# Active Directory answers searches with referrals to other domain controllers; following them
# is slow and often fails from outside the domain. Ignore them.
ldap.ignore-referrals=true

# Timeouts and cache of successful logins
ldap.timeout.connect=5s
ldap.timeout.read=30s
ldap.cache-ttl=1h
```

Notes:

* `ldap.user-bind-pattern` with the UPN form (`${USER}@corp.example.com`) is the most robust
  choice for AD: it works for users in any OU. Users then log in as `jdoe`, not as the full UPN.
* If users type the full UPN (`jdoe@corp.example.com`) use `${USER}` alone as the bind pattern and
  add a user mapping so that Trino's user name is the short form (see 1.3).
* `ldap.group-auth-pattern` requires `ldap.user-base-dn`. Nested groups are matched by AD with the
  `LDAP_MATCHING_RULE_IN_CHAIN` operator:
  `(memberOf:1.2.840.113556.1.4.1941:=CN=TrinoUsers,OU=Groups,DC=corp,DC=example,DC=com)`.
* For a **non-AD LDAP** (OpenLDAP) the bind pattern is typically
  `uid=${USER},ou=people,dc=example,dc=com` and the group filter uses `(uid=${USER})` and
  `memberOf` or `(member=uid=${USER},...)` on the group object.

### 1.3 User mapping (optional)

The name the driver sends in `X-Trino-User` must equal the authenticated user *after* Trino's user
mapping; otherwise Trino answers `403 Access Denied` ("User jdoe cannot impersonate user
jdoe@corp.example.com"). When users authenticate with their UPN but you want short names inside
Trino (for access control, `current_user`, resource groups), map the principal:

```properties
# etc/config.properties
http-server.authentication.password.user-mapping.pattern=(.*)(@.*)
```

The first capture group becomes the Trino user. With this mapping in place the driver must use the
short name as `UID` **and** the bind pattern must add the domain (`${USER}@corp.example.com`), or
the user must type the UPN and the driver's `UID` stays the UPN (then no mapping). Choose one
convention and document it for your users.

### 1.4 Restart and verify server-side

```sh
# after editing the properties files
bin/launcher restart

# from any machine: 401 means the authenticator is active; 200 means the login works
curl -i https://trino.corp.example.com:8443/v1/info
curl -i -u jdoe https://trino.corp.example.com:8443/v1/statement -X POST -d 'SELECT 1' \
     -H 'X-Trino-User: jdoe'
```

`etc/log.properties` with `io.trino.plugin.password=DEBUG` shows bind attempts and the DN
actually used.

## 2. Driver configuration

Minimal connection string:

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=8443;UID=jdoe;PWD=secret
```

That is enough: a `PWD` selects `AuthenticationType=LDAP`, and an authenticated mode selects
`SSL=1` (as long as `AllowInsecurePassword` is not set; with `AllowInsecurePassword=1` and no
`SSL=1` the connection would go to plain HTTP). `Port` must be given because the default with SSL
is `443`, not `8443`. Written out in full, with a private CA:

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=8443;AuthenticationType=LDAP;UID=jdoe;PWD=secret;SSL=1;SSLVerification=Full;TrustedCerts=C:\certs\corp-root-ca.pem
```

| Key | Value |
|---|---|
| `AuthenticationType` | `LDAP` (aliases `Password`, `Basic`, `UsernamePassword`, `UserPassword`, `1`). Implied by `PWD`. |
| `UID` | The login name expected by `ldap.user-bind-pattern`: `sAMAccountName` (`jdoe`) for a `${USER}@domain` or `CN=${USER},...` pattern; the UPN (`jdoe@corp.example.com`) for a bare `${USER}` pattern. It is also sent as `X-Trino-User`. |
| `PWD` | The AD password, taken verbatim (trailing spaces are kept). |
| `SSL` | `1` (implied unless `AllowInsecurePassword=1`). |
| `SSLVerification` | `Full` (default). `CA` when the certificate name does not match the host you use. |
| `TrustedCerts` | PEM file with the CA that issued the Trino HTTPS certificate, unless it is already in the system store (Windows) or the libcurl default bundle. See [SSL.md](SSL.md). |
| `Port` | `8443` or whatever `http-server.https.port` says. |

DSN form (`odbc.ini` / Windows setup dialog, password left to the application):

```ini
[TrinoAD]
Driver=Trino ODBC Driver
Host=trino.corp.example.com
Port=8443
AuthenticationType=LDAP
UID=jdoe
SSL=1
SSLVerification=Full
TrustedCerts=/etc/ssl/certs/corp-root-ca.pem
Catalog=hive
Schema=default
```

On Windows the setup dialog stores the password in the DSN only when *Save password* is ticked;
Power BI and most tools supply it through their own credential dialogs (`SQLConnect` /
`SQLDriverConnect` with `UID`/`PWD`), which override the DSN.

Related keys: `SessionUser` (run as another user after authenticating, requires impersonation
rights in Trino), `Roles` (`X-Trino-Role`), `AllowInsecurePassword` (plain HTTP, development only).

## 3. Troubleshooting

Set `LogLevel=4` (or the environment variable `TRINO_ODBC_LOG_LEVEL=4`) and read the driver log;
the Trino server log (`var/log/server.log`) shows the LDAP side.

| Symptom | Meaning | What to check |
|---|---|---|
| SQLSTATE `28000`, `Password authentication requires SSL` | `SSL=0` was given (or the DSN forces it) with a password. | Remove `SSL=0`; use the HTTPS port. `AllowInsecurePassword=1` is only for development. |
| Connection goes to `http://...:8080` although a password is set | `AllowInsecurePassword=1` is in the DSN or connection string: it disables the automatic TLS. | Remove it, or add `SSL=1` explicitly. |
| SQLSTATE `28000`, `LDAP/password authentication requires UID and PWD` | One of them is empty. | The application did not pass credentials; check the DSN/dialog. |
| SQLSTATE `28000`, `Authentication failed (HTTP 401)` | Trino rejected the bind. | Wrong password; account locked/expired; `UID` does not fit `ldap.user-bind-pattern` (short name vs UPN); user outside `ldap.user-base-dn`; group filter false. Server log shows the DN tried. |
| SQLSTATE `28000`, `Access denied (HTTP 403)` | Authentication succeeded but the request is not allowed. | `X-Trino-User` differs from the mapped principal (see 1.3); system access control (`etc/access-control.properties`) denies the user; `SessionUser` impersonation not permitted. |
| SQLSTATE `08001`, certificate verification error | TLS handshake failed. | `TrustedCerts`, host name vs certificate SAN, `SSLVerification`. See [SSL.md](SSL.md). |
| SQLSTATE `08001` / `HYT01`, connection timed out | Network path or wrong port. | `Port`, firewall to 8443, `ConnectTimeout`. |
| Login works with `curl -u` but not through the driver | Header mismatch. | `UID` must equal the Basic user name (the driver uses the same value for both); check for trailing spaces in the DSN. |
| Login is slow (seconds) | LDAP referrals or a slow group search. | `ldap.ignore-referrals=true`, `ldap.bind-dn` service account, `ldap.cache-ttl`. |
| `javax.naming.CommunicationException` / `PKIX path building failed` in `server.log` | Trino does not trust the domain-controller certificate. | `ldap.ssl.truststore.path` with the AD root CA. |
| `Password authentication requires HTTPS` in `server.log` | Trino received the password on the plain HTTP port. | Use the HTTPS port; or `http-server.authentication.allow-insecure-over-http=true` for development. |
| Account gets locked after a failed login | Repeated wrong password. | The driver never retries a 401/403 (retries apply to HTTP 502/503/504 and transient network failures only), so each application connect attempt is exactly one bind. Look for other tools or a saved DSN password. |
| Turkish/Unicode passwords fail | Encoding. | Use the Unicode API (`SQLDriverConnectW`) or `AnsiEncoding=utf8` (the default on macOS/Linux; Windows defaults to the system code page); the driver sends UTF-8 in the Basic header. |
| Password with trailing blanks fails | An unbraced connection-string value is trimmed. | Brace it: `PWD={secret }`. The driver keeps `PWD` from a DSN verbatim, but the driver manager's `odbc.ini` parser may already have trimmed it. |

### Testing outside Power BI

Linux/macOS (unixODBC):

```sh
isql -v TrinoAD jdoe 'secret'
```

macOS (iODBC):

```sh
iodbctest "DSN=TrinoAD;UID=jdoe;PWD=secret"
```

Windows (PowerShell, through the 64-bit Windows driver manager):

```powershell
$c = New-Object System.Data.Odbc.OdbcConnection "DSN=TrinoAD;UID=jdoe;PWD=secret"
$c.Open(); (New-Object System.Data.Odbc.OdbcCommand("SELECT current_user", $c)).ExecuteScalar(); $c.Close()
```
