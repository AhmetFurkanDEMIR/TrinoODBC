# Changelog

All notable changes to the Trino ODBC driver are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-11-02

First public release.

### Added

* ODBC 3.8 driver for Trino written in C++17: every core, level 1 and level 2 function in ANSI
  and Unicode (`W`) form, descriptors (ARD/APD/IRD/IPD), block cursors with column-wise and
  row-wise binding, chunked `SQLGetData`, data-at-execution parameters and the full conversion
  matrix. Forward-only, read-only cursors.
* Authentication: none, LDAP / Active Directory (HTTP Basic over TLS), JWT, Kerberos (SPNEGO),
  OAuth2 (external browser flow) and client certificates (mTLS).
* TLS: `Full` / `CA` / `None` verification modes, custom CA bundle, Windows certificate store,
  minimum TLS version, client certificates, HTTP and SOCKS proxies.
* Trino spooled protocol (`json`, `json+zstd`, `json+lz4`) with parallel segment download and
  bounded background prefetch; classic JSON protocol as fallback.
* Query cancellation that also stops the query on the Trino coordinator (`SQLCancel`, Power BI
  *Cancel*), plus the `IdleFetchTimeout` connection option.
* Catalog functions over `system.jdbc.*`, `SQLGetTypeInfo`, ODBC escape sequence translation,
  session properties, roles, extra credentials, client tags, `USE`, prepared statements and
  optional transactions.
* Default session time zone **Europe/Istanbul (UTC+03:00)**, editable in the Windows DSN dialog
  and through the `TimeZone` key (`TimeZone=system` uses the operating system time zone).
* Power Query custom connector (`TrinoODBC.mez`) for Power BI DirectQuery and query folding.
* Packages: Windows x64 NSIS installer (cross-built with mingw-w64), macOS Apple silicon `.pkg`
  and `.dmg` (iODBC), Linux `.deb`, `.rpm` and `.tar.gz` for x86_64 and arm64 (unixODBC), all
  branded with the Trino logo.
* Documentation in English and Turkish: installation per platform, Power BI, connection options,
  LDAP/AD, SSL, performance, types, troubleshooting and ODBC capabilities.

### Verified

* Installed and used on Windows x64 (including a full Power BI Desktop test), Linux x86_64 and
  arm64, and macOS Apple silicon.
* Unit tests and the integration suite pass on macOS and Linux against the Python mock server
  and a real Trino 483 over plain HTTP and over HTTPS with password authentication and the spooled
  protocol; an ASan/UBSan build passes on macOS.

[1.0.0]: https://github.com/AhmetFurkanDEMIR/TrinoODBC/releases/tag/v1.0.0
