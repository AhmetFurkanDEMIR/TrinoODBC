// Windows ODBC installer entry points (ConfigDSN / ConfigDriver, ANSI + Unicode) and the
// connection prompt used by SQLDriverConnect.
//
// Design notes:
//  * The dialog edits a flat map of canonical connection keys (ConnectionConfig::canonicalKey), not
//    ConnectionConfig fields: a DSN may contain keys the dialog has no control for (MaxRetries,
//    proxies, ...) and they must survive a round trip through the dialog untouched. The prompt path
//    (showConnectDialog) converts ConnectionConfig -> map with toConnectionString(true) /
//    parseConnectionString and back with applyAll().
//  * Registry access goes exclusively through the installer API (SQLWritePrivateProfileStringW,
//    SQLWriteDSNToIniW, ...) so user/system DSNs and the WOW64 redirection are handled by odbccp32.
//  * "Test connection" goes through the Driver Manager (odbc32.dll loaded dynamically) with the
//    assembled connection string: it exercises exactly the path an application will use, including
//    driver registration, instead of calling the driver's own entry points.
//  * Every entry point is a Win32 boundary: no exception may escape (installer errors are posted with
//    SQLPostInstallerErrorW instead).
#include "platform.h"

#if defined(TRINO_ODBC_WINDOWS)

#include "platform/win/resource.h"
#include "platform/win/setup.h"

#include "core/config.h"
#include "core/platform_setup.h"
#include "util/errors.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/unicode.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef ODBC_CONFIG_DRIVER
#  define ODBC_CONFIG_DRIVER 3
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#  define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace trino_odbc {
namespace {

static_assert(sizeof(SQLWCHAR) == sizeof(wchar_t), "Windows SQLWCHAR must be wchar_t sized");

constexpr const char* kLogComponent = "setup";
constexpr const wchar_t* kOdbcIni = L"ODBC.INI";
constexpr const wchar_t* kOdbcInstIni = L"ODBCINST.INI";
constexpr const char* kDefaultDriverName = "Trino ODBC Driver";
constexpr int kMaxDsnLength = 32;  // SQL_MAX_DSN_LENGTH

// ---- string conversion ----------------------------------------------------------------------------

std::wstring toWide(std::string_view utf8) {
  const WString w = utf8ToWide(utf8);
  return std::wstring(reinterpret_cast<const wchar_t*>(w.data()), w.size());
}

std::string fromWide(const wchar_t* s, size_t len) {
  if (!s) return std::string();
  return wideToUtf8(reinterpret_cast<const SQLWCHAR*>(s), static_cast<SQLLEN>(len));
}

std::string fromWide(const wchar_t* s) { return s ? fromWide(s, std::wcslen(s)) : std::string(); }
std::string fromWide(const std::wstring& s) { return fromWide(s.data(), s.size()); }

std::string fromAnsi(const char* s) {
  if (!s) return std::string();
  return narrowToUtf8(reinterpret_cast<const SQLCHAR*>(s), static_cast<SQLLEN>(std::strlen(s)), AnsiEncoding::System);
}

/// Connection-string value quoting identical to ConnectionConfig::toConnectionString (braces when the
/// value contains separators or special characters; '}' doubled inside braces).
std::string quoteConnValue(std::string_view v) {
  const bool needsBraces = !v.empty() && (v.find_first_of(";{}[]()=,?*!@\"'") != std::string_view::npos ||
                                          v.front() == ' ' || v.back() == ' ');
  if (!needsBraces) return std::string(v);
  std::string out = "{";
  for (const char c : v) {
    if (c == '}') out += "}}";
    else out += c;
  }
  out += "}";
  return out;
}

// ---- installer API helpers ------------------------------------------------------------------------

/// SQLGetPrivateProfileStringW with a growing buffer. `entry == nullptr` enumerates the section's
/// key names as a NUL-separated list. Returns the raw characters without the final terminator.
std::wstring profileRead(const std::wstring& section, const wchar_t* entry, const wchar_t* file) {
  std::vector<wchar_t> buf(1024);
  for (;;) {
    const int n = SQLGetPrivateProfileStringW(section.c_str(), entry, L"", buf.data(), static_cast<int>(buf.size()), file);
    if (n <= 0) return std::wstring();
    // The installer reports cb-1 (value) / cb-2 (enumeration) when the buffer was too small; a nearly
    // full buffer therefore means "retry with a bigger one".
    if (static_cast<size_t>(n) + 2 < buf.size()) return std::wstring(buf.data(), static_cast<size_t>(n));
    if (buf.size() >= (1u << 20)) return std::wstring(buf.data(), static_cast<size_t>(n));
    buf.assign(buf.size() * 4, L'\0');
  }
}

std::vector<std::wstring> splitNulList(const std::wstring& raw) {
  std::vector<std::wstring> items;
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = start;
    while (end < raw.size() && raw[end] != L'\0') ++end;
    if (end > start) items.emplace_back(raw.data() + start, end - start);
    start = end + 1;
  }
  return items;
}

void postInstallerError(DWORD code, std::string_view message) {
  TRINO_LOG_WARN(kLogComponent, "installer error %lu: %.*s", static_cast<unsigned long>(code),
                 static_cast<int>(message.size()), message.data());
  SQLPostInstallerErrorW(code, toWide(message).c_str());
}

std::wstring modulePath() {
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    const DWORD n = GetModuleFileNameW(driverModuleHandle(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return std::wstring();
    if (n < buf.size() - 1) return std::wstring(buf.data(), n);
    if (buf.size() >= 32768) return std::wstring(buf.data(), n);
    buf.resize(buf.size() * 2);
  }
}

std::wstring fileNameOf(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

/// Name under which this DLL is registered in ODBCINST.INI ("" when not registered). Needed to build
/// a DRIVER= connection string for "Test connection" when the caller did not tell us the name.
std::string findOwnDriverName() {
  const std::wstring own = modulePath();
  if (own.empty()) return std::string();
  const std::wstring ownFile = fileNameOf(own);
  std::string byFileName;
  for (const std::wstring& name : splitNulList(profileRead(L"ODBC Drivers", nullptr, kOdbcInstIni))) {
    const std::wstring path = profileRead(name, L"Driver", kOdbcInstIni);
    if (path.empty()) continue;
    if (lstrcmpiW(path.c_str(), own.c_str()) == 0) return fromWide(name);
    // A copy of the DLL registered from another directory still identifies the same driver.
    if (byFileName.empty() && lstrcmpiW(fileNameOf(path).c_str(), ownFile.c_str()) == 0) byFileName = fromWide(name);
  }
  return byFileName;
}

bool looksLikePath(std::string_view s) {
  return s.find('\\') != std::string_view::npos || s.find('/') != std::string_view::npos || iendsWith(s, ".dll");
}

// ---- value model ------------------------------------------------------------------------------------

/// Everything the dialog edits or must preserve for one DSN.
struct SetupValues {
  KeyValueMap known;                       // canonical key -> value
  KeyValueMap unknown;                     // keys the driver does not know (kept verbatim)
  std::vector<std::string> staleEntries;   // alias-spelled entries found in the DSN (rewritten canonically)
};

std::string valueOf(const SetupValues& v, const char* canonicalKey) {
  const auto it = v.known.find(canonicalKey);
  return it == v.known.end() ? std::string() : it->second;
}

bool boolOf(const SetupValues& v, const char* canonicalKey, bool defaultValue) {
  bool b = defaultValue;
  const std::string text = valueOf(v, canonicalKey);
  if (!text.empty() && parseBool(text, &b)) return b;
  return defaultValue;
}

void assignValue(SetupValues& v, std::string_view key, std::string_view value) {
  const std::string_view k = trim(key);
  if (k.empty()) return;
  const std::string canonical = ConnectionConfig::canonicalKey(k);
  if (canonical.empty()) {
    v.unknown[std::string(k)] = std::string(value);
    return;
  }
  v.known[canonical] = std::string(value);
}

/// Parses the `key=value` items of a ConfigDSN attribute list.
SetupValues parseAttributes(const std::vector<std::string>& items) {
  SetupValues v;
  for (const std::string& item : items) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      assignValue(v, item, std::string());
      continue;
    }
    assignValue(v, item.substr(0, eq), item.substr(eq + 1));
  }
  return v;
}

std::vector<std::string> attributeList(const wchar_t* attributes) {
  std::vector<std::string> items;
  for (const wchar_t* p = attributes; p && *p;) {
    const size_t len = std::wcslen(p);
    items.push_back(fromWide(p, len));
    p += len + 1;
  }
  return items;
}

std::vector<std::string> attributeList(const char* attributes) {
  std::vector<std::string> items;
  for (const char* p = attributes; p && *p;) {
    const size_t len = std::strlen(p);
    items.push_back(narrowToUtf8(reinterpret_cast<const SQLCHAR*>(p), static_cast<SQLLEN>(len), AnsiEncoding::System));
    p += len + 1;
  }
  return items;
}

/// Names of every entry currently stored in the DSN section (used to delete only what exists).
std::vector<std::string> dsnEntryNames(const std::string& dsn) {
  std::vector<std::string> names;
  for (const std::wstring& w : splitNulList(profileRead(toWide(dsn), nullptr, kOdbcIni))) names.push_back(fromWide(w));
  return names;
}

/// Merges the stored DSN section into `v` (values already present, i.e. from the attribute string, win).
void loadDsnSection(SetupValues& v, const std::string& dsn) {
  const std::wstring wdsn = toWide(dsn);
  for (const std::string& key : dsnEntryNames(dsn)) {
    // "Driver" holds the DLL path written by SQLWriteDSNToIni; the section name is the DSN itself.
    if (iequals(key, "DSN") || iequals(key, "Driver")) continue;
    const std::string value = fromWide(profileRead(wdsn, toWide(key).c_str(), kOdbcIni));
    const std::string canonical = ConnectionConfig::canonicalKey(key);
    if (canonical.empty()) {
      if (v.unknown.find(key) == v.unknown.end()) v.unknown[key] = value;
      continue;
    }
    // A Simba/JDBC-style alias (Server=, Database=, ...) is rewritten under its canonical name on
    // save so the DSN never ends up with two spellings of the same option.
    if (!iequals(canonical, key)) v.staleEntries.push_back(key);
    if (v.known.find(canonical) == v.known.end()) v.known[canonical] = value;
  }
}

/// True when a data source called `dsn` exists (in the scope the installer is currently configured
/// for); *ownerDriver receives the driver it is registered with. The section itself is checked too
/// because a hand-edited registry may lack the [ODBC Data Sources] entry.
bool dsnExists(const std::string& dsn, std::string* ownerDriver) {
  const std::wstring owner = profileRead(L"ODBC Data Sources", toWide(dsn).c_str(), kOdbcIni);
  if (ownerDriver) *ownerDriver = fromWide(owner);
  return !owner.empty() || !dsnEntryNames(dsn).empty();
}

bool writeEntry(const std::wstring& section, const std::string& key, const std::string& value) {
  const std::wstring wkey = toWide(key);
  const std::wstring wvalue = toWide(value);
  return SQLWritePrivateProfileStringW(section.c_str(), wkey.c_str(), wvalue.c_str(), kOdbcIni) != FALSE;
}

void deleteEntry(const std::wstring& section, const std::string& key) {
  const std::wstring wkey = toWide(key);
  if (!SQLWritePrivateProfileStringW(section.c_str(), wkey.c_str(), nullptr, kOdbcIni)) {
    TRINO_LOG_WARN(kLogComponent, "could not remove DSN entry '%s'", key.c_str());
  }
}

/// Writes the DSN section. `previousDsn` (ConfigDSN rename) is removed first. Returns false with a
/// message in *error when the installer refused a write.
bool saveDsn(const SetupValues& v, const std::string& dsn, const std::string& previousDsn, const std::string& driverName,
             bool savePassword, std::string* error) {
  if (!previousDsn.empty() && !iequals(previousDsn, dsn)) {
    if (!SQLRemoveDSNFromIniW(toWide(previousDsn).c_str())) {
      *error = "Could not remove the previous data source name '" + previousDsn + "'.";
      return false;
    }
  }
  std::vector<std::string> existing = dsnEntryNames(dsn);
  auto exists = [&existing](const std::string& key) {
    for (const auto& e : existing) {
      if (iequals(e, key)) return true;
    }
    return false;
  };

  const std::wstring wdsn = toWide(dsn);
  if (!SQLWriteDSNToIniW(wdsn.c_str(), toWide(driverName).c_str())) {
    *error = "Could not register the data source name '" + dsn + "' (driver '" + driverName + "').";
    return false;
  }
  for (const std::string& key : ConnectionConfig::knownKeys()) {
    if (iequals(key, "DSN") || iequals(key, "Driver")) continue;
    std::string value;
    const auto it = v.known.find(key);
    if (it != v.known.end()) value = it->second;
    if (iequals(key, "PWD") && !savePassword) value.clear();
    if (value.empty()) {
      if (exists(key)) deleteEntry(wdsn, key);
      continue;
    }
    if (!writeEntry(wdsn, key, value)) {
      *error = "Could not write the '" + key + "' entry of data source '" + dsn + "'.";
      return false;
    }
  }
  for (const std::string& stale : v.staleEntries) {
    if (exists(stale)) deleteEntry(wdsn, stale);
  }
  for (const auto& [key, value] : v.unknown) {
    if (value.empty()) {
      if (exists(key)) deleteEntry(wdsn, key);
      continue;
    }
    if (!writeEntry(wdsn, key, value)) {
      *error = "Could not write the '" + key + "' entry of data source '" + dsn + "'.";
      return false;
    }
  }
  TRINO_LOG_INFO(kLogComponent, "saved DSN '%s' (driver '%s', password %s)", dsn.c_str(), driverName.c_str(),
                 savePassword ? "stored" : "not stored");
  return true;
}

// ---- dialog ----------------------------------------------------------------------------------------

enum class DialogMode { AddDsn, ConfigDsn, Connect };
enum class DialogResult { Ok, Cancelled, Failed };

struct DialogContext {
  DialogMode mode = DialogMode::AddDsn;
  SetupValues values;
  std::string driverName;   // registered driver name for "Test connection" ("" = unknown)
  std::string originalDsn;  // ConfigDsn: the name before editing
  bool savePassword = false;
  bool passwordWasStored = false;
  // SSL / AuthenticationType bookkeeping: finalize() derives both when they are absent, so the dialog
  // must not turn a derived value into a stored one just because it displays it.
  bool sslExplicit = false;   // SSL was present when the dialog opened
  bool authExplicit = false;  // AuthenticationType was present when the dialog opened
  bool sslTouched = false;    // the user clicked the SSL checkbox
  bool authTouched = false;   // the user changed the authentication combo
  bool initialized = false;   // fillDialog finished (suppresses notifications sent while filling)
};

struct ComboEntry {
  const wchar_t* label;
  const char* value;
};

constexpr ComboEntry kAuthEntries[] = {
    {L"None (no authentication)", "None"},
    {L"LDAP / Active Directory (user name and password)", "LDAP"},
    {L"Kerberos (SPNEGO)", "Kerberos"},
    {L"JWT / access token", "JWT"},
    {L"OAuth2 (external browser)", "OAuth2"},
    {L"Client certificate (mutual TLS)", "Certificate"},
};
constexpr ComboEntry kSslEntries[] = {
    {L"Full - verify certificate chain and host name", "Full"},
    {L"CA - verify certificate chain only", "CA"},
    {L"None - trust any certificate (insecure)", "None"},
};
constexpr ComboEntry kLogEntries[] = {
    {L"0 - Off", "0"},   {L"1 - Error", "1"}, {L"2 - Warning", "2"},
    {L"3 - Info", "3"},  {L"4 - Debug", "4"}, {L"5 - Trace", "5"},
};

/// Suggestions of the editable "Time zone" drop-down (any other IANA name or an offset such as
/// +05:30 may be typed). The driver default (kDefaultSessionTimeZone) is listed first. The note next
/// to the box (IDC_TIME_ZONE_NOTE) explains the default, so the hints box does not repeat it.
constexpr const wchar_t* kTimeZoneSuggestions[] = {
    L"Europe/Istanbul",  L"UTC",           L"Europe/London",    L"Europe/Berlin",
    L"Europe/Paris",     L"Europe/Moscow", L"Asia/Dubai",       L"Asia/Kolkata",
    L"Asia/Singapore",   L"Asia/Tokyo",    L"Australia/Sydney", L"America/New_York",
    L"America/Chicago",  L"America/Denver", L"America/Los_Angeles", L"America/Sao_Paulo",
    L"system",
};

constexpr const wchar_t* kHintsText =
    L"Session properties, extra credentials and roles: key=value pairs separated by ';' "
    L"(e.g. query_max_run_time=1h;query_priority=2). "
    L"Roles: catalog=ROLE{name} or system=ALL.\r\n\r\n"
    L"Certificate files are PEM; the client certificate may also be PKCS#12 (.p12 / .pfx). "
    L"Leave the CA file empty to use the Windows certificate store.\r\n\r\n"
    L"Password (LDAP) authentication requires SSL. Empty fields keep the driver defaults. "
    L"Log path may be a folder (a trino_odbc_<pid>.log file is created in it).";

std::string getText(HWND dlg, int id) {
  HWND h = GetDlgItem(dlg, id);
  if (!h) return std::string();
  const int len = GetWindowTextLengthW(h);
  if (len <= 0) return std::string();
  std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
  const int n = GetWindowTextW(h, &buf[0], len + 1);
  return fromWide(buf.data(), n > 0 ? static_cast<size_t>(n) : 0);
}

std::string getTrimmedText(HWND dlg, int id) { return std::string(trim(getText(dlg, id))); }

void setText(HWND dlg, int id, std::string_view utf8) { SetDlgItemTextW(dlg, id, toWide(utf8).c_str()); }

bool isChecked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }
void setChecked(HWND dlg, int id, bool on) { CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }
void enableControl(HWND dlg, int id, bool on) { EnableWindow(GetDlgItem(dlg, id), on ? TRUE : FALSE); }

void fillCombo(HWND dlg, int id, const ComboEntry* entries, size_t count, size_t selected) {
  HWND combo = GetDlgItem(dlg, id);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < count; ++i) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entries[i].label));
  SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected < count ? selected : 0), 0);
}

const char* comboValue(HWND dlg, int id, const ComboEntry* entries, size_t count) {
  const LRESULT sel = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
  if (sel == CB_ERR || sel < 0 || static_cast<size_t>(sel) >= count) return entries[0].value;
  return entries[static_cast<size_t>(sel)].value;
}

size_t authIndexFor(std::string_view text) {
  AuthType t = AuthType::None;
  if (text.empty() || !parseAuthType(text, &t)) return 0;
  for (size_t i = 0; i < sizeof(kAuthEntries) / sizeof(kAuthEntries[0]); ++i) {
    if (iequals(kAuthEntries[i].value, authTypeName(t))) return i;
  }
  return 0;
}

size_t sslIndexFor(std::string_view text) {
  SslVerification v = SslVerification::Full;
  if (text.empty() || !parseSslVerification(text, &v)) return 0;
  for (size_t i = 0; i < sizeof(kSslEntries) / sizeof(kSslEntries[0]); ++i) {
    if (iequals(kSslEntries[i].value, sslVerificationName(v))) return i;
  }
  return 0;
}

size_t logIndexFor(std::string_view text) {
  int64_t v = 0;
  if (text.empty() || !parseInt64(trim(text), &v) || v < 0 || v > 5) return 0;
  return static_cast<size_t>(v);
}

/// Fills the editable "Time zone" drop-down with kTimeZoneSuggestions and shows `configured`, or the
/// driver default (kDefaultSessionTimeZone, Europe/Istanbul = UTC+3) when the DSN / connection string
/// has no TimeZone. The default is shown as a real value, not a placeholder: pressing OK writes it
/// to the DSN, so the zone in use is visible in the registry and can be changed there as well.
void fillTimeZoneCombo(HWND dlg, std::string_view configured) {
  HWND combo = GetDlgItem(dlg, IDC_TIME_ZONE);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(combo, CB_LIMITTEXT, 64, 0);
  for (const wchar_t* zone : kTimeZoneSuggestions) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(zone));
  const std::string_view trimmed = trim(configured);
  const std::wstring shown = toWide(trimmed.empty() ? std::string_view(kDefaultSessionTimeZone) : trimmed);
  const LRESULT exact = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(shown.c_str()));
  if (exact != CB_ERR) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(exact), 0);
  SetWindowTextW(combo, shown.c_str());  // also covers zones that are not in the list
  const std::wstring note = L"Default: UTC+3 T\u00fcrkiye / Istanbul (" + toWide(kDefaultSessionTimeZone) +
                            L"). Change it if your data needs another zone; 'system' = Windows zone.";
  SetDlgItemTextW(dlg, IDC_TIME_ZONE_NOTE, note.c_str());
}

/// Sets the Trino logo (IDI_TRINOODBC) as the dialog's title-bar / Alt+Tab icon.
void setDialogIcons(HWND dlg) {
  const HINSTANCE module = driverModuleHandle();
  const HANDLE big = LoadImageW(module, MAKEINTRESOURCEW(IDI_TRINOODBC), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
  const HANDLE small = LoadImageW(module, MAKEINTRESOURCEW(IDI_TRINOODBC), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  if (big) SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
  if (small) SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
}

/// Releases the icons set by setDialogIcons (they were not loaded with LR_SHARED).
void destroyDialogIcons(HWND dlg) {
  for (const WPARAM which : {static_cast<WPARAM>(ICON_BIG), static_cast<WPARAM>(ICON_SMALL)}) {
    const HICON old = reinterpret_cast<HICON>(SendMessageW(dlg, WM_SETICON, which, 0));
    if (old) DestroyIcon(old);
  }
}

/// Enables the credential / TLS controls that make sense for the selected authentication and SSL.
void updateEnabledState(HWND dlg, const DialogContext& ctx) {
  const std::string auth = comboValue(dlg, IDC_AUTH_TYPE, kAuthEntries, sizeof(kAuthEntries) / sizeof(kAuthEntries[0]));
  const bool ldap = iequals(auth, "LDAP");
  const bool jwt = iequals(auth, "JWT");
  enableControl(dlg, IDC_PASSWORD, ldap);
  enableControl(dlg, IDC_SAVE_PASSWORD, ldap && ctx.mode != DialogMode::Connect);
  enableControl(dlg, IDC_TOKEN, jwt);
  const bool ssl = isChecked(dlg, IDC_SSL);
  enableControl(dlg, IDC_SSL_VERIFICATION, ssl);
  enableControl(dlg, IDC_CA_FILE, ssl);
}

void fillDialog(HWND dlg, DialogContext& ctx) {
  const SetupValues& v = ctx.values;
  ctx.authExplicit = !trim(valueOf(v, "AuthenticationType")).empty();
  ctx.sslExplicit = !trim(valueOf(v, "SSL")).empty();
  // Show what the connection will really use: stored values where present, finalize()'s derivation
  // (Port=443 / credentials imply TLS, PWD implies LDAP, a token implies JWT) otherwise.
  const DerivedTransport shown = deriveTransport(v.known, true, true);
  const size_t shownAuthIndex = authIndexFor(authTypeName(shown.auth));
  SetWindowTextW(dlg, ctx.mode == DialogMode::Connect ? L"Trino ODBC Driver - Connect"
                                                       : L"Trino ODBC Driver - Data Source Setup");
  SendDlgItemMessageW(dlg, IDC_DSN_NAME, EM_SETLIMITTEXT, kMaxDsnLength, 0);
  SendDlgItemMessageW(dlg, IDC_PORT, EM_SETLIMITTEXT, 5, 0);
  SendDlgItemMessageW(dlg, IDC_TARGET_RESULT_SIZE, EM_SETLIMITTEXT, 3, 0);

  setText(dlg, IDC_DSN_NAME, valueOf(v, "DSN"));
  setText(dlg, IDC_DESCRIPTION, valueOf(v, "Description"));
  setText(dlg, IDC_HOST, valueOf(v, "Host"));
  setText(dlg, IDC_PORT, valueOf(v, "Port"));
  setText(dlg, IDC_CATALOG, valueOf(v, "Catalog"));
  setText(dlg, IDC_SCHEMA, valueOf(v, "Schema"));
  fillCombo(dlg, IDC_AUTH_TYPE, kAuthEntries, sizeof(kAuthEntries) / sizeof(kAuthEntries[0]), shownAuthIndex);
  setText(dlg, IDC_USER, valueOf(v, "UID"));
  setText(dlg, IDC_PASSWORD, valueOf(v, "PWD"));
  setChecked(dlg, IDC_SAVE_PASSWORD, ctx.savePassword);
  setText(dlg, IDC_TOKEN, valueOf(v, "AccessToken"));
  setChecked(dlg, IDC_SSL, shown.ssl);
  fillCombo(dlg, IDC_SSL_VERIFICATION, kSslEntries, sizeof(kSslEntries) / sizeof(kSslEntries[0]),
            sslIndexFor(valueOf(v, "SSLVerification")));
  setText(dlg, IDC_CA_FILE, valueOf(v, "TrustedCerts"));
  setText(dlg, IDC_CLIENT_CERT, valueOf(v, "ClientCert"));
  setText(dlg, IDC_CLIENT_KEY, valueOf(v, "ClientKey"));
  setText(dlg, IDC_CLIENT_KEY_PASSWORD, valueOf(v, "ClientKeyPassword"));
  setText(dlg, IDC_SESSION_PROPERTIES, valueOf(v, "SessionProperties"));
  setText(dlg, IDC_EXTRA_CREDENTIALS, valueOf(v, "ExtraCredentials"));
  setText(dlg, IDC_ROLES, valueOf(v, "Roles"));
  fillTimeZoneCombo(dlg, valueOf(v, "TimeZone"));
  setText(dlg, IDC_SOURCE, valueOf(v, "Source"));
  setChecked(dlg, IDC_SPOOLING, boolOf(v, "Spooling", true));
  setText(dlg, IDC_TARGET_RESULT_SIZE, valueOf(v, "TargetResultSize"));
  fillCombo(dlg, IDC_LOG_LEVEL, kLogEntries, sizeof(kLogEntries) / sizeof(kLogEntries[0]),
            logIndexFor(valueOf(v, "LogLevel")));
  setText(dlg, IDC_LOG_PATH, valueOf(v, "LogPath"));
  SetDlgItemTextW(dlg, IDC_HINTS, kHintsText);
  setText(dlg, IDC_STATUS, "");

  if (ctx.mode == DialogMode::Connect) {
    // The data source identity is not editable while connecting; the password is only kept for this
    // connection so "save" makes no sense.
    enableControl(dlg, IDC_DSN_NAME, false);
    enableControl(dlg, IDC_DESCRIPTION, false);
    ShowWindow(GetDlgItem(dlg, IDC_SAVE_PASSWORD), SW_HIDE);
    SetDlgItemTextW(dlg, IDOK, L"Connect");
  }
  updateEnabledState(dlg, ctx);

  // Put the caret on the first field that still needs input.
  int focus = IDC_HOST;
  if (ctx.mode != DialogMode::Connect && valueOf(v, "DSN").empty()) focus = IDC_DSN_NAME;
  else if (valueOf(v, "Host").empty()) focus = IDC_HOST;
  else if (shownAuthIndex == 1 && valueOf(v, "PWD").empty()) focus = IDC_PASSWORD;
  SetFocus(GetDlgItem(dlg, focus));
  ctx.initialized = true;
}

/// Copies the controls back into ctx.values (keys without a control are left untouched).
void readDialog(HWND dlg, DialogContext& ctx) {
  SetupValues& v = ctx.values;
  auto put = [&v](const char* key, std::string value) { v.known[key] = std::move(value); };
  if (ctx.mode != DialogMode::Connect) put("DSN", getTrimmedText(dlg, IDC_DSN_NAME));
  put("Description", getTrimmedText(dlg, IDC_DESCRIPTION));
  put("Host", getTrimmedText(dlg, IDC_HOST));
  put("Port", getTrimmedText(dlg, IDC_PORT));
  put("Catalog", getTrimmedText(dlg, IDC_CATALOG));
  put("Schema", getTrimmedText(dlg, IDC_SCHEMA));
  put("UID", getTrimmedText(dlg, IDC_USER));
  put("PWD", getText(dlg, IDC_PASSWORD));  // secrets verbatim: passwords may end in spaces
  put("AccessToken", getTrimmedText(dlg, IDC_TOKEN));
  put("SSLVerification", comboValue(dlg, IDC_SSL_VERIFICATION, kSslEntries, sizeof(kSslEntries) / sizeof(kSslEntries[0])));
  put("TrustedCerts", getTrimmedText(dlg, IDC_CA_FILE));
  put("ClientCert", getTrimmedText(dlg, IDC_CLIENT_CERT));
  put("ClientKey", getTrimmedText(dlg, IDC_CLIENT_KEY));
  put("ClientKeyPassword", getText(dlg, IDC_CLIENT_KEY_PASSWORD));
  put("SessionProperties", getTrimmedText(dlg, IDC_SESSION_PROPERTIES));
  put("ExtraCredentials", getTrimmedText(dlg, IDC_EXTRA_CREDENTIALS));
  put("Roles", getTrimmedText(dlg, IDC_ROLES));
  put("TimeZone", getTrimmedText(dlg, IDC_TIME_ZONE));
  put("Source", getTrimmedText(dlg, IDC_SOURCE));
  put("Spooling", isChecked(dlg, IDC_SPOOLING) ? "1" : "0");
  put("TargetResultSize", getTrimmedText(dlg, IDC_TARGET_RESULT_SIZE));
  put("LogLevel", comboValue(dlg, IDC_LOG_LEVEL, kLogEntries, sizeof(kLogEntries) / sizeof(kLogEntries[0])));
  put("LogPath", getTrimmedText(dlg, IDC_LOG_PATH));
  // Last, because whether SSL / AuthenticationType stay implicit depends on every other key.
  TransportChoice choice;
  choice.auth = comboValue(dlg, IDC_AUTH_TYPE, kAuthEntries, sizeof(kAuthEntries) / sizeof(kAuthEntries[0]));
  choice.authExplicit = ctx.authExplicit;
  choice.authTouched = ctx.authTouched;
  choice.ssl = isChecked(dlg, IDC_SSL);
  choice.sslExplicit = ctx.sslExplicit;
  choice.sslTouched = ctx.sslTouched;
  applyTransportChoice(v.known, choice);
  ctx.savePassword = ctx.mode == DialogMode::Connect ? true : isChecked(dlg, IDC_SAVE_PASSWORD);
}

/// Keeps an implicit SSL checkbox in step with what finalize() would derive from the fields edited so
/// far (choosing LDAP or typing port 443 turns TLS on), until the user operates the checkbox itself.
void refreshImpliedSsl(HWND dlg, const DialogContext& ctx) {
  if (!ctx.initialized || ctx.sslExplicit || ctx.sslTouched) return;
  DialogContext probe = ctx;
  readDialog(dlg, probe);
  setChecked(dlg, IDC_SSL, deriveTransport(probe.values.known, true, false).ssl);
}

void showError(HWND dlg, int focusControl, std::string_view message) {
  MessageBoxW(dlg, toWide(message).c_str(), L"Trino ODBC Driver", MB_OK | MB_ICONEXCLAMATION);
  if (focusControl) SetFocus(GetDlgItem(dlg, focusControl));
}

bool isValidDsnName(const std::string& dsn) {
  if (dsn.empty() || utf8CodePoints(dsn) > static_cast<size_t>(kMaxDsnLength)) return false;
  return SQLValidDSNW(toWide(dsn).c_str()) != FALSE;
}

/// Field-level checks; returns false after telling the user what to fix. `forConnect` applies the
/// stricter rules of an immediate connection (credentials must be complete).
bool validateValues(HWND dlg, DialogContext& ctx, bool forConnect) {
  SetupValues& v = ctx.values;
  if (ctx.mode != DialogMode::Connect) {
    const std::string dsn = valueOf(v, "DSN");
    if (dsn.empty()) {
      showError(dlg, IDC_DSN_NAME, "Enter a data source name.");
      return false;
    }
    if (!isValidDsnName(dsn)) {
      showError(dlg, IDC_DSN_NAME,
                "The data source name is invalid. It must be at most 32 characters long and must not contain "
                "[ ] { } ( ) , ; ? * = ! @ or a backslash.");
      return false;
    }
  }
  if (valueOf(v, "Host").empty()) {
    showError(dlg, IDC_HOST, "Enter the host name of the Trino coordinator.");
    return false;
  }
  const std::string port = valueOf(v, "Port");
  if (!port.empty()) {
    int64_t n = 0;
    if (!parseInt64(port, &n) || n < 1 || n > 65535) {
      showError(dlg, IDC_PORT, "Port must be a number between 1 and 65535 (leave it empty for 8080 / 443).");
      return false;
    }
  }
  const std::string targetSize = valueOf(v, "TargetResultSize");
  if (!targetSize.empty()) {
    int64_t n = 0;
    if (!parseInt64(targetSize, &n) || n < 1 || n > 128) {
      showError(dlg, IDC_TARGET_RESULT_SIZE, "Target result size must be between 1 and 128 MB.");
      return false;
    }
  }
  // Let the driver's own parser judge the k=v lists so the dialog and the connect path agree.
  for (const char* key : {"SessionProperties", "ExtraCredentials", "Roles"}) {
    const std::string text = valueOf(v, key);
    if (text.empty()) continue;
    ConnectionConfig probe;
    std::string error;
    if (!probe.applyKeyValue(key, text, &error)) {
      const int control = iequals(key, "SessionProperties") ? IDC_SESSION_PROPERTIES
                          : iequals(key, "ExtraCredentials") ? IDC_EXTRA_CREDENTIALS
                                                             : IDC_ROLES;
      showError(dlg, control, error.empty() ? std::string(key) + " is malformed." : error);
      return false;
    }
  }

  // Effective values, i.e. what finalize() makes of stored and implied keys alike.
  const DerivedTransport effective = deriveTransport(v.known, true, true);
  const AuthType auth = effective.auth;
  const bool ssl = effective.ssl;
  if (forConnect) {
    if (auth == AuthType::Ldap && valueOf(v, "UID").empty()) {
      showError(dlg, IDC_USER, "LDAP authentication requires a user name.");
      return false;
    }
    if (auth == AuthType::Ldap && valueOf(v, "PWD").empty()) {
      showError(dlg, IDC_PASSWORD, "LDAP authentication requires a password.");
      return false;
    }
    if (auth == AuthType::Jwt && valueOf(v, "AccessToken").empty() && valueOf(v, "AccessTokenFile").empty()) {
      showError(dlg, IDC_TOKEN, "JWT authentication requires an access token.");
      return false;
    }
    if (auth == AuthType::Certificate && valueOf(v, "ClientCert").empty()) {
      showError(dlg, IDC_CLIENT_CERT, "Certificate authentication requires a client certificate file.");
      return false;
    }
  }
  if (auth == AuthType::Ldap && !ssl && !boolOf(v, "AllowInsecurePassword", false)) {
    // Mirror ConnectionConfig::validate(): the driver refuses Basic auth over plain HTTP unless the
    // user explicitly opts in, so offer the opt-in here instead of failing later with 28000.
    const int answer = MessageBoxW(
        dlg,
        L"Password authentication without SSL sends the password unencrypted over the network, and the driver "
        L"refuses it unless AllowInsecurePassword=1 is set.\r\n\r\n"
        L"Yes: set AllowInsecurePassword=1 for this data source (development only).\r\n"
        L"No: go back and enable SSL.",
        L"Trino ODBC Driver", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
      SetFocus(GetDlgItem(dlg, IDC_SSL));
      return false;
    }
    v.known["AllowInsecurePassword"] = "1";
  }
  return true;
}

/// ConfigDSN spec: when the name entered in the dialog belongs to another existing data source, the
/// user must confirm before it is replaced (SQLWriteDSNToIni silently deletes the existing section).
/// Returns false (back to the dialog) unless the user agreed. Editing a DSN under its own name, in any
/// letter case, is not an overwrite.
bool confirmOverwrite(HWND dlg, const DialogContext& ctx) {
  if (ctx.mode == DialogMode::Connect) return true;
  const std::string dsn = valueOf(ctx.values, "DSN");
  std::string owner;
  if (!ctx.originalDsn.empty() && iequals(ctx.originalDsn, dsn)) return true;  // no registry probe needed
  if (!needsOverwriteConfirmation(ctx.originalDsn, dsn, dsnExists(dsn, &owner))) return true;
  std::string message = "A data source named '" + dsn + "' already exists";
  if (!owner.empty()) message += " (driver: " + owner + ")";
  message += ".\r\n\r\nReplace it with this Trino data source? Its current settings are lost.";
  const int answer = MessageBoxW(dlg, toWide(message).c_str(), L"Trino ODBC Driver",
                                 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (answer == IDYES) {
    TRINO_LOG_INFO(kLogComponent, "user confirmed replacing existing DSN '%s'", dsn.c_str());
    return true;
  }
  SetFocus(GetDlgItem(dlg, IDC_DSN_NAME));
  SendDlgItemMessageW(dlg, IDC_DSN_NAME, EM_SETSEL, 0, -1);
  return false;
}

// ---- Test connection (through the Driver Manager) ---------------------------------------------------

std::string buildTestConnectionString(const DialogContext& ctx) {
  std::string out;
  auto add = [&out](std::string_view key, std::string_view value) {
    if (value.empty()) return;
    if (!out.empty()) out += ';';
    out += key;
    out += '=';
    out += quoteConnValue(value);
  };
  if (!ctx.driverName.empty()) {
    add("Driver", ctx.driverName);
  } else if (ctx.mode == DialogMode::Connect && !valueOf(ctx.values, "DSN").empty()) {
    // Not registered under a name we can find: let the DM resolve the driver from the DSN; every
    // value below overrides what the DSN stores.
    add("DSN", valueOf(ctx.values, "DSN"));
  }
  for (const std::string& key : ConnectionConfig::knownKeys()) {
    if (iequals(key, "DSN") || iequals(key, "Driver")) continue;
    add(key, valueOf(ctx.values, key.c_str()));
  }
  for (const auto& [key, value] : ctx.values.unknown) add(key, value);
  return out;
}

struct DriverManager {
  HMODULE lib = nullptr;
  SQLRETURN(SQL_API* allocHandle)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*) = nullptr;
  SQLRETURN(SQL_API* setEnvAttr)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) = nullptr;
  SQLRETURN(SQL_API* driverConnectW)(SQLHDBC, SQLHWND, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                     SQLUSMALLINT) = nullptr;
  SQLRETURN(SQL_API* getDiagRecW)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLWCHAR*, SQLINTEGER*, SQLWCHAR*, SQLSMALLINT,
                                  SQLSMALLINT*) = nullptr;
  SQLRETURN(SQL_API* getInfoW)(SQLHDBC, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*) = nullptr;
  SQLRETURN(SQL_API* disconnect)(SQLHDBC) = nullptr;
  SQLRETURN(SQL_API* freeHandle)(SQLSMALLINT, SQLHANDLE) = nullptr;

  DriverManager() = default;
  DriverManager(const DriverManager&) = delete;
  DriverManager& operator=(const DriverManager&) = delete;
  ~DriverManager() {
    if (lib) FreeLibrary(lib);
  }

  bool load() {
    // System32 only: a bare-name LoadLibrary would also search the current directory and PATH.
    lib = LoadLibraryExW(L"odbc32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return false;
    // GetProcAddress returns FARPROC; the double cast is the documented way to obtain a typed pointer.
    auto resolve = [this](const char* name) -> void* {
      return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetProcAddress(lib, name)));
    };
    allocHandle = reinterpret_cast<decltype(allocHandle)>(resolve("SQLAllocHandle"));
    setEnvAttr = reinterpret_cast<decltype(setEnvAttr)>(resolve("SQLSetEnvAttr"));
    driverConnectW = reinterpret_cast<decltype(driverConnectW)>(resolve("SQLDriverConnectW"));
    getDiagRecW = reinterpret_cast<decltype(getDiagRecW)>(resolve("SQLGetDiagRecW"));
    getInfoW = reinterpret_cast<decltype(getInfoW)>(resolve("SQLGetInfoW"));
    disconnect = reinterpret_cast<decltype(disconnect)>(resolve("SQLDisconnect"));
    freeHandle = reinterpret_cast<decltype(freeHandle)>(resolve("SQLFreeHandle"));
    return allocHandle && setEnvAttr && driverConnectW && getDiagRecW && getInfoW && disconnect && freeHandle;
  }

  std::string diagnostics(SQLSMALLINT handleType, SQLHANDLE handle) const {
    std::string out;
    for (SQLSMALLINT rec = 1; rec <= 5; ++rec) {
      SQLWCHAR state[6] = {};
      SQLWCHAR message[1024] = {};
      SQLINTEGER native = 0;
      SQLSMALLINT len = 0;
      const SQLRETURN rc = getDiagRecW(handleType, handle, rec, state, &native, message, 1024, &len);
      if (!SQL_SUCCEEDED(rc)) break;
      if (!out.empty()) out += "\r\n";
      out += "[" + wideToUtf8(state, static_cast<SQLLEN>(SQL_NTS)) + "] " +
             wideToUtf8(message, static_cast<SQLLEN>(SQL_NTS));
      if (native != 0) out += " (native " + std::to_string(native) + ")";
    }
    return out;
  }
};

void runTestConnection(HWND dlg, DialogContext& ctx) {
  if (ctx.driverName.empty() && (ctx.mode != DialogMode::Connect || valueOf(ctx.values, "DSN").empty())) {
    showError(dlg, 0,
              "The driver is not registered with the ODBC Driver Manager under a name, so a test connection "
              "cannot be made. Register the driver (installer or register_driver.ps1) and try again.");
    return;
  }
  DriverManager dm;
  if (!dm.load()) {
    showError(dlg, 0, "odbc32.dll (the ODBC Driver Manager) could not be loaded.");
    return;
  }

  const std::string connStr = buildTestConnectionString(ctx);
  const std::string host = valueOf(ctx.values, "Host");
  TRINO_LOG_INFO(kLogComponent, "test connection to '%s' through the driver manager", host.c_str());

  setText(dlg, IDC_STATUS, "Connecting to " + host + " ...");
  UpdateWindow(dlg);
  HCURSOR previousCursor = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));  // IDC_WAIT
  enableControl(dlg, IDC_TEST_CONNECTION, false);
  enableControl(dlg, IDOK, false);

  std::string resultText;
  bool ok = false;
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  if (!SQL_SUCCEEDED(dm.allocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
    resultText = "SQLAllocHandle(SQL_HANDLE_ENV) failed.";
  } else {
    dm.setEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_OV_ODBC3)), 0);
    if (!SQL_SUCCEEDED(dm.allocHandle(SQL_HANDLE_DBC, env, &dbc))) {
      resultText = "SQLAllocHandle(SQL_HANDLE_DBC) failed:\r\n" + dm.diagnostics(SQL_HANDLE_ENV, env);
    } else {
      std::wstring wconn = toWide(connStr);
      SQLWCHAR outConn[1024] = {};
      SQLSMALLINT outLen = 0;
      const SQLRETURN rc = dm.driverConnectW(dbc, dlg, reinterpret_cast<SQLWCHAR*>(&wconn[0]), SQL_NTS, outConn, 1024,
                                             &outLen, SQL_DRIVER_NOPROMPT);
      if (SQL_SUCCEEDED(rc)) {
        ok = true;
        SQLWCHAR version[256] = {};
        SQLSMALLINT versionBytes = 0;
        std::string versionText;
        if (SQL_SUCCEEDED(dm.getInfoW(dbc, SQL_DBMS_VER, version, static_cast<SQLSMALLINT>(sizeof(version)), &versionBytes))) {
          versionText = wideToUtf8(version, static_cast<SQLLEN>(SQL_NTS));
        }
        resultText = "Connection successful.";
        if (!versionText.empty()) resultText += "\r\nTrino version: " + versionText;
        if (rc == SQL_SUCCESS_WITH_INFO) {
          const std::string info = dm.diagnostics(SQL_HANDLE_DBC, dbc);
          if (!info.empty()) resultText += "\r\n\r\nWarnings:\r\n" + info;
        }
        dm.disconnect(dbc);
      } else {
        resultText = "Connection failed.\r\n\r\n" + dm.diagnostics(SQL_HANDLE_DBC, dbc);
      }
      dm.freeHandle(SQL_HANDLE_DBC, dbc);
    }
    dm.freeHandle(SQL_HANDLE_ENV, env);
  }

  enableControl(dlg, IDC_TEST_CONNECTION, true);
  enableControl(dlg, IDOK, true);
  SetCursor(previousCursor);
  setText(dlg, IDC_STATUS, ok ? "Connection successful." : "Connection failed.");
  TRINO_LOG_INFO(kLogComponent, "test connection %s", ok ? "succeeded" : "failed");
  MessageBoxW(dlg, toWide(resultText).c_str(), L"Trino ODBC Driver - Test connection",
              MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

// ---- dialog procedure ----------------------------------------------------------------------------

/// Runs a handler and reports (instead of propagating) anything it throws: this is a Win32 callback.
template <typename Fn>
void guardedUi(HWND dlg, const char* what, Fn&& fn) {
  try {
    fn();
  } catch (const std::bad_alloc&) {
    MessageBoxW(dlg, L"Out of memory.", L"Trino ODBC Driver", MB_OK | MB_ICONERROR);
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLogComponent, "%s failed: %s", what, e.what());
    MessageBoxW(dlg, toWide(e.what()).c_str(), L"Trino ODBC Driver", MB_OK | MB_ICONERROR);
  } catch (...) {
    MessageBoxW(dlg, L"Unexpected error.", L"Trino ODBC Driver", MB_OK | MB_ICONERROR);
  }
}

INT_PTR CALLBACK setupDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  DialogContext* ctx = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
  switch (msg) {
    case WM_INITDIALOG: {
      ctx = reinterpret_cast<DialogContext*>(lParam);
      SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      setDialogIcons(dlg);
      guardedUi(dlg, "dialog initialisation", [&] { fillDialog(dlg, *ctx); });
      return FALSE;  // focus was set explicitly in fillDialog
    }
    case WM_COMMAND: {
      if (!ctx) return FALSE;
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      switch (id) {
        case IDOK:
          guardedUi(dlg, "OK", [&] {
            readDialog(dlg, *ctx);
            if (validateValues(dlg, *ctx, ctx->mode == DialogMode::Connect) && confirmOverwrite(dlg, *ctx)) {
              EndDialog(dlg, IDOK);
            }
          });
          return TRUE;
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
        case IDC_TEST_CONNECTION:
          guardedUi(dlg, "test connection", [&] {
            readDialog(dlg, *ctx);
            if (validateValues(dlg, *ctx, true)) runTestConnection(dlg, *ctx);
          });
          return TRUE;
        case IDC_AUTH_TYPE:
          if (code == CBN_SELCHANGE) {
            guardedUi(dlg, "authentication change", [&] {
              if (ctx->initialized) ctx->authTouched = true;
              refreshImpliedSsl(dlg, *ctx);
              updateEnabledState(dlg, *ctx);
            });
          }
          return TRUE;
        case IDC_SSL:
          if (code == BN_CLICKED) {
            if (ctx->initialized) ctx->sslTouched = true;
            updateEnabledState(dlg, *ctx);
          }
          return TRUE;
        case IDC_PORT:
          if (code == EN_CHANGE) {
            guardedUi(dlg, "port change", [&] {
              refreshImpliedSsl(dlg, *ctx);
              updateEnabledState(dlg, *ctx);
            });
            return TRUE;
          }
          return FALSE;
        default:
          return FALSE;
      }
    }
    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
    case WM_DESTROY:
      destroyDialogIcons(dlg);
      return FALSE;
    default:
      return FALSE;
  }
}

DialogResult runDialog(HWND parent, DialogContext& ctx) {
  const INT_PTR result = DialogBoxParamW(driverModuleHandle(), MAKEINTRESOURCEW(IDD_TRINO_DSN_DIALOG), parent,
                                         setupDialogProc, reinterpret_cast<LPARAM>(&ctx));
  if (result == IDOK) return DialogResult::Ok;
  if (result == IDCANCEL) return DialogResult::Cancelled;
  TRINO_LOG_ERROR(kLogComponent, "DialogBoxParamW failed (result %lld, error %lu)", static_cast<long long>(result),
                  static_cast<unsigned long>(GetLastError()));
  return DialogResult::Failed;
}

// ---- ConfigDSN core ---------------------------------------------------------------------------------

bool configDsn(HWND hwnd, WORD request, std::string driverName, const std::vector<std::string>& attributes) {
  SetupValues values = parseAttributes(attributes);
  const std::string dsn = valueOf(values, "DSN");
  if (driverName.empty()) driverName = kDefaultDriverName;
  TRINO_LOG_INFO(kLogComponent, "ConfigDSN request=%u driver='%s' dsn='%s' silent=%d", static_cast<unsigned>(request),
                 driverName.c_str(), dsn.c_str(), hwnd == nullptr ? 1 : 0);

  switch (request) {
    case ODBC_REMOVE_DSN: {
      if (dsn.empty()) {
        postInstallerError(ODBC_ERROR_INVALID_DSN, "ODBC_REMOVE_DSN requires a DSN attribute.");
        return false;
      }
      if (!SQLRemoveDSNFromIniW(toWide(dsn).c_str())) {
        postInstallerError(ODBC_ERROR_REMOVE_DSN_FAILED, "Could not remove data source '" + dsn + "'.");
        return false;
      }
      TRINO_LOG_INFO(kLogComponent, "removed DSN '%s'", dsn.c_str());
      return true;
    }
    case ODBC_ADD_DSN:
    case ODBC_CONFIG_DSN:
      break;
    default:
      postInstallerError(ODBC_ERROR_INVALID_REQUEST_TYPE, "Unsupported ConfigDSN request type.");
      return false;
  }

  DialogContext ctx;
  ctx.mode = request == ODBC_ADD_DSN ? DialogMode::AddDsn : DialogMode::ConfigDsn;
  ctx.driverName = driverName;
  if (request == ODBC_CONFIG_DSN) {
    if (dsn.empty()) {
      postInstallerError(ODBC_ERROR_INVALID_DSN, "ODBC_CONFIG_DSN requires a DSN attribute.");
      return false;
    }
    ctx.originalDsn = dsn;
    loadDsnSection(values, dsn);
  }
  ctx.passwordWasStored = !valueOf(values, "PWD").empty();
  ctx.savePassword = ctx.passwordWasStored;
  ctx.values = std::move(values);

  if (hwnd == nullptr) {
    // Silent mode (SQLConfigDataSource without a parent window): the attribute string must be
    // complete; nothing is prompted.
    const std::string name = valueOf(ctx.values, "DSN");
    if (!isValidDsnName(name)) {
      postInstallerError(ODBC_ERROR_INVALID_DSN, "Missing or invalid DSN attribute '" + name + "'.");
      return false;
    }
    if (valueOf(ctx.values, "Host").empty()) {
      postInstallerError(ODBC_ERROR_INVALID_KEYWORD_VALUE, "The Host attribute is required.");
      return false;
    }
    ctx.savePassword = true;  // a password given in the attributes is meant to be stored
  } else {
    switch (runDialog(hwnd, ctx)) {
      case DialogResult::Cancelled:
        TRINO_LOG_INFO(kLogComponent, "ConfigDSN cancelled by the user");
        return false;  // per spec: FALSE without an installer error
      case DialogResult::Failed:
        postInstallerError(ODBC_ERROR_REQUEST_FAILED, "The data source dialog could not be displayed.");
        return false;
      case DialogResult::Ok:
        break;
    }
  }

  std::string error;
  if (!saveDsn(ctx.values, valueOf(ctx.values, "DSN"), ctx.originalDsn, driverName, ctx.savePassword, &error)) {
    postInstallerError(ODBC_ERROR_REQUEST_FAILED, error);
    return false;
  }
  return true;
}

/// Runs an installer entry point body, converting exceptions into installer errors.
template <typename Fn>
BOOL installerBoundary(const char* what, Fn&& fn) {
  try {
    return fn() ? TRUE : FALSE;
  } catch (const std::bad_alloc&) {
    postInstallerError(ODBC_ERROR_OUT_OF_MEM, "Out of memory.");
  } catch (const DriverError& e) {
    postInstallerError(ODBC_ERROR_REQUEST_FAILED, std::string(what) + ": " + e.message());
  } catch (const std::exception& e) {
    postInstallerError(ODBC_ERROR_REQUEST_FAILED, std::string(what) + ": " + e.what());
  } catch (...) {
    postInstallerError(ODBC_ERROR_GENERAL_ERR, std::string(what) + ": unexpected error.");
  }
  return FALSE;
}

/// Copies an installer message into the caller's buffer with ODBC truncation semantics
/// (*outLen = full length, buffer always NUL-terminated when it has room).
template <typename CharT>
void copyInstallerMessage(const std::basic_string<CharT>& text, CharT* buf, WORD bufMax, WORD* outLen) {
  if (outLen) *outLen = static_cast<WORD>(text.size() > 0xFFFF ? 0xFFFF : text.size());
  if (!buf || bufMax == 0) return;
  const size_t n = text.size() < static_cast<size_t>(bufMax) - 1 ? text.size() : static_cast<size_t>(bufMax) - 1;
  std::memcpy(buf, text.data(), n * sizeof(CharT));
  buf[n] = CharT{};
}

/// Shared ConfigDriver logic; returns the message for the caller and true on success.
bool configDriver(WORD request, const std::string& driverName, std::string* message) {
  switch (request) {
    case ODBC_INSTALL_DRIVER:
      *message = driverName + " " + kDriverVersion + " installed.";
      break;
    case ODBC_REMOVE_DRIVER:
      *message = driverName + " removed.";
      break;
    case ODBC_CONFIG_DRIVER:
      // Nothing is configurable at driver level; every option is a DSN / connection-string key.
      *message = driverName + " " + kDriverVersion + " has no driver-level options.";
      break;
    default:
      postInstallerError(ODBC_ERROR_INVALID_REQUEST_TYPE, "Unsupported ConfigDriver request type.");
      return false;
  }
  TRINO_LOG_INFO(kLogComponent, "ConfigDriver request=%u: %s", static_cast<unsigned>(request), message->c_str());
  return true;
}

}  // namespace

// ---- connection prompt -------------------------------------------------------------------------------

bool showConnectDialog(HWND parent, ConnectionConfig& config) {
  DialogContext ctx;
  ctx.mode = DialogMode::Connect;
  // Everything the application supplied, secrets included, so the dialog shows the real state.
  // SSL / AuthenticationType only when the application or DSN gave them: the config is not finalized
  // yet, so its SSL flag is a placeholder and applying it back would disable the implied TLS.
  const KeyValueMap current = promptDialogValues(config);
  for (const auto& [key, value] : current) assignValue(ctx.values, key, value);
  ctx.passwordWasStored = !config.password.empty();
  ctx.savePassword = true;
  ctx.driverName = config.driver;
  if (ctx.driverName.empty() || looksLikePath(ctx.driverName)) ctx.driverName = findOwnDriverName();

  switch (runDialog(parent, ctx)) {
    case DialogResult::Cancelled:
      TRINO_LOG_INFO(kLogComponent, "connection prompt cancelled by the user");
      return false;
    case DialogResult::Failed:
      throw DriverError(sqlstate::kDialogFailed, "The connection dialog could not be displayed");
    case DialogResult::Ok:
      break;
  }

  // Keys whose default is non-empty must not be blanked by an empty control (empty numeric / boolean
  // values are already treated as "not set" by applyKeyValue).
  static const char* const kKeepDefaultWhenEmpty[] = {
      "Source", "AuthenticationType", "SSLVerification", "KerberosServiceName", "MinTLSVersion",
      "QueryDataEncoding", "TimestampWithTimeZoneMode", "TimeWithTimeZoneMode", "IntervalMode",
      "ExternalAuthRedirectHandler", "AnsiEncoding",
  };
  KeyValueMap apply;
  for (const auto& [key, value] : ctx.values.known) {
    if (iequals(key, "DSN") || iequals(key, "Driver")) continue;
    if (value.empty()) {
      bool keep = false;
      for (const char* k : kKeepDefaultWhenEmpty) {
        if (iequals(k, key)) keep = true;
      }
      if (keep) continue;
    }
    apply[key] = value;
  }
  for (const auto& [key, value] : ctx.values.unknown) apply[key] = value;
  config.applyAll(apply);  // throws DriverError on a malformed value; the caller's guard reports it
  TRINO_LOG_INFO(kLogComponent, "connection prompt accepted (host '%s', auth %s)", config.host.c_str(),
                 authTypeName(config.auth));
  return true;
}

}  // namespace trino_odbc

// ---- exported installer entry points ------------------------------------------------------------------

extern "C" {

BOOL INSTAPI ConfigDSNW(HWND hwndParent, WORD fRequest, LPCWSTR lpszDriver, LPCWSTR lpszAttributes) {
  return trino_odbc::installerBoundary("ConfigDSNW", [&] {
    return trino_odbc::configDsn(hwndParent, fRequest, trino_odbc::fromWide(lpszDriver),
                                 trino_odbc::attributeList(lpszAttributes));
  });
}

BOOL INSTAPI ConfigDSN(HWND hwndParent, WORD fRequest, LPCSTR lpszDriver, LPCSTR lpszAttributes) {
  return trino_odbc::installerBoundary("ConfigDSN", [&] {
    return trino_odbc::configDsn(hwndParent, fRequest, trino_odbc::fromAnsi(lpszDriver),
                                 trino_odbc::attributeList(lpszAttributes));
  });
}

BOOL INSTAPI ConfigDriverW(HWND /*hwndParent*/, WORD fRequest, LPCWSTR lpszDriver, LPCWSTR /*lpszArgs*/, LPWSTR lpszMsg,
                           WORD cbMsgMax, WORD* pcbMsgOut) {
  return trino_odbc::installerBoundary("ConfigDriverW", [&] {
    std::string driverName = trino_odbc::fromWide(lpszDriver);
    if (driverName.empty()) driverName = trino_odbc::kDefaultDriverName;
    std::string message;
    const bool ok = trino_odbc::configDriver(fRequest, driverName, &message);
    trino_odbc::copyInstallerMessage(trino_odbc::toWide(message), lpszMsg, cbMsgMax, pcbMsgOut);
    return ok;
  });
}

BOOL INSTAPI ConfigDriver(HWND /*hwndParent*/, WORD fRequest, LPCSTR lpszDriver, LPCSTR /*lpszArgs*/, LPSTR lpszMsg,
                          WORD cbMsgMax, WORD* pcbMsgOut) {
  return trino_odbc::installerBoundary("ConfigDriver", [&] {
    std::string driverName = trino_odbc::fromAnsi(lpszDriver);
    if (driverName.empty()) driverName = trino_odbc::kDefaultDriverName;
    std::string message;
    const bool ok = trino_odbc::configDriver(fRequest, driverName, &message);
    trino_odbc::copyInstallerMessage(trino_odbc::utf8ToNarrow(message, trino_odbc::AnsiEncoding::System), lpszMsg,
                                     cbMsgMax, pcbMsgOut);
    return ok;
  });
}

}  // extern "C"

#endif  // TRINO_ODBC_WINDOWS
