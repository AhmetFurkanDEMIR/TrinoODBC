# TLS / SSL

The driver talks to the Trino coordinator over HTTP or HTTPS through libcurl. This page explains
how HTTPS is switched on, how the server certificate is verified, how to trust a corporate CA,
how client certificates work, and what to do in development setups.

## When HTTPS is used

`SSL=1` selects `https://`. You rarely need to set it. When `SSL` is not given, HTTPS is used

* when `Port=443`, or
* when the authentication type is anything other than `None` (LDAP, JWT, Kerberos, OAuth2,
  Certificate), **unless** `AllowInsecurePassword=1` is set.

An explicit `SSL=0` or `SSL=1` is always honoured. Credentials over plain HTTP (`SSL=0`, or
`AllowInsecurePassword=1` without `SSL=1`) are only accepted with `AllowInsecurePassword=1`, and
never for `Certificate` authentication (see the end of this page).

When `Port` is not given it defaults to `443` with SSL and `8080` without. Trino's HTTPS listener
is usually configured on `8443` (`http-server.https.port`), so set `Port` explicitly in that case.

The driver never follows HTTP redirects, so `Host`/`Port` must point at the coordinator (or the
load balancer in front of it) directly.

## Verification modes

| `SSLVerification` | Chain checked | Host name checked | Use |
|---|---|---|---|
| `Full` (default) | yes | yes | Production. |
| `CA` | yes | no | Certificate issued by a trusted CA but for a different name (e.g. connecting through an IP address or an internal alias). |
| `None` | no | no | Development only. The driver logs a warning at level 2 at every connect. |

Accepted spellings (case-insensitive): `Full` / `verify-full` / `verify_full` / `verifyfull` /
`on` / `true` / `yes` / `1`; `CA` / `verify-ca` / `verify_ca` / `verifyca` / `chain`;
`None` / `off` / `false` / `no` / `0` / `disable` / `disabled` / `insecure`. Anything else fails
the connect with `HY000`.

Starburst/Simba DSNs are honoured: `AllowSelfSignedServerCert=1` maps to `SSLVerification=None`,
`AllowHostNameCNMismatch=1` lowers `Full` to `CA` (it leaves `None` alone); the value `0` changes
nothing. An explicit `SSLVerification` given in the connection string is never undone by one of
these flags coming from the DSN.

In libcurl terms: `CURLOPT_SSL_VERIFYPEER = (mode != None)` and
`CURLOPT_SSL_VERIFYHOST = (mode == Full ? 2 : 0)`.

## Which CA certificates are trusted

1. `TrustedCerts=<file>` (aliases `SSLCA`, `CAFile`, `SSLTrustStorePath`): a PEM bundle. This is
   the recommended way to trust a corporate or private CA. Several certificates may be
   concatenated in one file.
2. `CAPath=<dir>` (alias `SSLCAPath`): a directory of PEM files named by subject hash
   (`openssl rehash <dir>`).
3. `UseSystemTrustStore=1` (default): on **Windows** the driver adds the Windows certificate
   store (`CURLSSLOPT_NATIVE_CA`, compiled in only when the libcurl headers define it), so a CA
   imported into *Trusted Root Certification Authorities* (for the machine or the user) should be
   accepted without any DSN change. The Windows x64 package (the mingw-w64 build, whose libcurl
   uses Schannel) has connected over HTTPS in a real environment; `TrustedCerts` remains the
   fallback when a CA is not in the store. On **macOS and
   Linux** libcurl's default CA bundle is used (Homebrew/OpenSSL `cert.pem`,
   `/etc/ssl/certs/ca-certificates.crt`, ...); the key has no effect there.

`TrustedCerts` and `CAPath` are passed to libcurl as `CURLOPT_CAINFO` / `CURLOPT_CAPATH`. Some
TLS back-ends (for example a libcurl built against Secure Transport only) ignore them; the driver
logs a warning at level 2 when that happens.

### Getting the CA certificate in PEM form

From a Java keystore / PKCS#12 used by Trino (`http-server.https.keystore.path`):

```sh
# PKCS#12
openssl pkcs12 -in keystore.p12 -nokeys -out server-chain.pem
# JKS: export with keytool, then convert DER -> PEM
keytool -exportcert -keystore keystore.jks -alias trino -file trino.der
openssl x509 -inform der -in trino.der -out trino.pem
```

From the running server (shows the chain; take the last certificate, the root, or the whole
chain):

```sh
openssl s_client -connect trino.corp.example.com:8443 -showcerts </dev/null
```

Windows: open the certificate, *Details* > *Copy to File* > *Base-64 encoded X.509 (.CER)*; the
result is PEM and can be used as `TrustedCerts` directly.

## Minimum TLS version

`MinTLSVersion=1.2` (default) or `1.3` sets `CURLOPT_SSLVERSION` (`TLSv1_2` / `TLSv1_3`).
Accepted spellings: `1.2`, `tlsv1.2`, `tls1.2`, `tls12`, `12` and the same for 1.3. Trino's
default Jetty configuration accepts TLS 1.2 and 1.3.

## Client certificates (mutual TLS)

| Key | Meaning |
|---|---|
| `ClientCert` (`SSLCert`, `SSLKeyStorePath`) | PEM certificate, or a PKCS#12 file when the name ends in `.p12` or `.pfx` (libcurl `CURLOPT_SSLCERTTYPE=P12`). |
| `ClientKey` (`SSLKey`) | PEM private key (not needed with PKCS#12). Ignored without `ClientCert`. |
| `ClientKeyPassword` (`SSLKeyPassword`, `SSLKeyStorePassword`) | Key or PKCS#12 passphrase, taken verbatim. Ignored without `ClientCert`. |

A client certificate can be combined with any authentication type (for example an LDAP cluster
that also requires client certificates at the load balancer). With
`AuthenticationType=Certificate` the certificate *is* the authentication
(`http-server.authentication.type=CERTIFICATE` on the server): no `UID` is required and no
`Authorization` header is sent; Trino derives the user from the certificate subject (`X-Trino-User`
is sent only when `UID` is given). `Certificate` authentication always requires SSL;
`AllowInsecurePassword` does not relax that. Mutual TLS has not been exercised against a real
Trino in this project.

Example:

```
Driver={Trino ODBC Driver};Host=trino.corp.example.com;Port=443;AuthenticationType=Certificate;ClientCert=C:\Users\jdoe\trino\client.pfx;ClientKeyPassword=changeit
```

## Self-signed development servers

Preferred: trust the self-signed certificate explicitly. Export it (see above) and point
`TrustedCerts` at it; keep `SSLVerification=Full` if the certificate carries the host name you
connect to (SAN), otherwise `CA`.

```
Host=localhost;Port=18443;UID=alice;PWD=secret;SSL=1;SSLVerification=Full;TrustedCerts=/path/to/ca.pem
```

Quick and dirty: `SSLVerification=None` (or `AllowSelfSignedServerCert=1`). Every connect writes
`SSLVerification=None: the server certificate of <host> is not verified` to the log at level 2.

## Disabling TLS

Plain HTTP is only sensible for an unauthenticated development cluster:

```
Host=trino-dev;Port=8080;UID=analyst;SSL=0
```

Sending a password or token over plain HTTP (`SSL=0`) is refused with SQLSTATE `28000`
("Password authentication requires SSL ...", "Token authentication requires SSL ...",
"Kerberos/OAuth2 authentication requires SSL ...") unless `AllowInsecurePassword=1` is set; the
server must also accept it (`http-server.authentication.allow-insecure-over-http=true`).
`AllowInsecurePassword=1` alone already switches the automatic TLS off: without an explicit `SSL=1`
the connection then goes to `http://<host>:8080` (or the given `Port`). Certificate authentication
is always refused without SSL.

## Proxies

`HttpProxy=host:port` tunnels HTTPS through an HTTP proxy (`CONNECT`; `http://` is prepended when
no scheme is given); `SocksProxy=host:port` uses SOCKS5 with remote name resolution (`socks5h://`
is prepended when no scheme is given). The two are mutually exclusive (`HY024`). `ProxyUser` /
`ProxyPassword` supply proxy credentials. Certificate verification still happens against the Trino
server's certificate, not the proxy's.

## Diagnosing TLS failures

Set `LogLevel=4` and look for the libcurl message in the log. Typical cases:

| Message (SQLSTATE `08001` at connect, `08S01` later) | Cause | Fix |
|---|---|---|
| `SSL peer certificate or SSH remote key was not OK` / `unable to get local issuer certificate` | The CA is unknown to the driver. | `TrustedCerts=<ca.pem>`, or import the CA into the Windows store. |
| `SSL: no alternative certificate subject name matches target host name` | Certificate issued for another name. | Connect with the name in the certificate, or `SSLVerification=CA`. |
| `SSL certificate problem: self signed certificate` | Self-signed server. | Trust the certificate explicitly (see above). |
| `Received HTTP/0.9 when not allowed` or `Unsupported protocol` | `SSL=1` against a plain-HTTP port (or the reverse). | Check `Port` / `SSL`. |
| `error:0A000102:SSL routines::unsupported protocol` | Server only offers TLS below `MinTLSVersion`. | Upgrade the server; do not lower security. |
| `could not load PEM client certificate` | Wrong `ClientCert` path/format. | Use PEM or `.p12`/`.pfx`; check `ClientKeyPassword`. |

`curl -v --cacert ca.pem https://host:port/v1/info` reproduces the handshake outside the driver
with the same library.

## What has been verified

On macOS (iODBC) and Ubuntu (unixODBC) the driver was tested against a real Trino 483 coordinator
over HTTPS with a self-signed certificate, in all three verification modes (`Full` with
`TrustedCerts`, `CA`, `None`), with password (LDAP-style Basic) authentication and with the spooled
protocol (`json+zstd` / `json+lz4` segments stored in MinIO). In that setup the spooling manager
needs `fs.segment.encryption=false`, because MinIO is reached over plain HTTP and rejects SSE-C
encrypted segments; see `tests/docker/README.md`.

On Windows x64 the installed package (the mingw-w64 cross build, whose statically linked libcurl
uses Schannel and so verifies the server certificate against the Windows certificate store)
connected over HTTPS in a real environment. The native MSVC build with the `curl[schannel]`
libcurl that `packaging/windows/build.ps1` installs through vcpkg uses the same TLS back-end but
has not been compiled in this project. Client certificates (mTLS) have not been tested against a
real server on any platform.
