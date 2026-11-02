// Process-wide file logger. One mutex serialises sink management and writes so lines from concurrent
// statements never interleave; every line is flushed immediately because the most valuable log is the
// one written right before the host application crashed.
#include "util/log.h"

#include "platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#if defined(TRINO_ODBC_WINDOWS)
#  include <share.h>  // _SH_DENYNO
#endif
#if defined(TRINO_ODBC_UNIX)
#  include <pthread.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(TRINO_ODBC_LINUX)
#    include <sys/syscall.h>
#  endif
#endif

namespace trino_odbc {

namespace {

constexpr int kMinLevel = static_cast<int>(LogLevel::Off);
constexpr int kMaxLevel = static_cast<int>(LogLevel::Trace);

#if defined(TRINO_ODBC_WINDOWS)
std::wstring utf8ToWinWide(std::string_view s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
  return out;
}

std::string winWideToUtf8(const wchar_t* s, int len) {
  if (!s || len <= 0) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
  return out;
}
#endif

/// Environment lookup returning UTF-8; empty when unset. Windows reads the wide environment so
/// non-ASCII log directories survive.
std::string envVar(const char* name) {
#if defined(TRINO_ODBC_WINDOWS)
  const std::wstring wname = utf8ToWinWide(name);
  const DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (needed == 0) return std::string();
  std::vector<wchar_t> buf(static_cast<size_t>(needed) + 1, L'\0');
  const DWORD got = GetEnvironmentVariableW(wname.c_str(), buf.data(), needed + 1);
  if (got == 0 || got > needed) return std::string();
  return winWideToUtf8(buf.data(), static_cast<int>(got));
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

bool isDirectory(const std::string& path) {
  if (path.empty()) return false;
  const char last = path.back();
  if (last == '/' || last == '\\') return true;
#if defined(TRINO_ODBC_WINDOWS)
  const DWORD attrs = GetFileAttributesW(utf8ToWinWide(path).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
#endif
}

std::string defaultLogDirectory() {
#if defined(TRINO_ODBC_WINDOWS)
  wchar_t buf[MAX_PATH + 1];
  const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  if (n == 0 || n > MAX_PATH) return std::string(".");
  return winWideToUtf8(buf, static_cast<int>(n));
#else
  const std::string tmp = envVar("TMPDIR");
  return tmp.empty() ? std::string("/tmp") : tmp;
#endif
}

unsigned long currentProcessId() {
#if defined(TRINO_ODBC_WINDOWS)
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

/// Native thread id (what a debugger / Activity Monitor shows), not std::thread::id's opaque hash.
unsigned long long currentThreadId() {
#if defined(TRINO_ODBC_WINDOWS)
  return static_cast<unsigned long long>(GetCurrentThreadId());
#elif defined(TRINO_ODBC_MACOS)
  uint64_t tid = 0;
  if (pthread_threadid_np(nullptr, &tid) != 0) tid = reinterpret_cast<uintptr_t>(pthread_self());
  return static_cast<unsigned long long>(tid);
#elif defined(SYS_gettid)
  return static_cast<unsigned long long>(::syscall(SYS_gettid));
#else
  return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

std::FILE* openAppend(const std::string& path) {
#if defined(TRINO_ODBC_WINDOWS)
  // Shared open: _wfopen_s opens write handles exclusively, which would lock out every other process
  // loading the driver with the same LogPath (e.g. several Power BI mashup containers) and anyone
  // trying to read the log while the host runs. Append mode seeks to the end before every write, so
  // concurrent writers do not overwrite each other's lines.
  return _wfsopen(utf8ToWinWide(path).c_str(), L"ab", _SH_DENYNO);
#else
  return std::fopen(path.c_str(), "ab");
#endif
}

/// "2026-09-27 12:00:00.123" in local time; local because that is what the person reading the file
/// compares against their application's clock.
void formatTimestamp(char* buf, size_t cap) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t secs = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
#if defined(TRINO_ODBC_WINDOWS)
  localtime_s(&tm, &secs);
#else
  localtime_r(&secs, &tm);
#endif
  std::snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
}

}  // namespace

Logger& Logger::global() {
  static Logger instance;
  return instance;
}

const char* Logger::levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Off: return "OFF";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Trace: return "TRACE";
  }
  return "?";
}

std::string Logger::mask(std::string_view secret) {
  if (secret.empty()) return std::string();
  if (secret.size() < 8) return "***";
  std::string out;
  out.reserve(7);
  out.append(secret.substr(0, 2));
  out.append("***");
  out.append(secret.substr(secret.size() - 2));
  return out;
}

std::string Logger::path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return path_;
}

void Logger::configure(int level, std::string_view path) {
  if (level < kMinLevel) level = kMinLevel;
  if (level > kMaxLevel) level = kMaxLevel;
  std::lock_guard<std::mutex> lock(mutex_);
  // The environment is an operator override: a DSN must not be able to lower or raise it.
  if (!envLevelForced_ && level > level_.load(std::memory_order_relaxed)) {
    level_.store(level, std::memory_order_relaxed);
  }
  if (path_.empty() && !path.empty()) path_ = std::string(path);
  if (level_.load(std::memory_order_relaxed) > kMinLevel && !file_) openSinkLocked();
}

void Logger::configureFromEnvironment() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (envApplied_) return;
  envApplied_ = true;
  const std::string levelText = envVar("TRINO_ODBC_LOG_LEVEL");
  if (!levelText.empty()) {
    char* end = nullptr;
    const long v = std::strtol(levelText.c_str(), &end, 10);
    if (end && *end == '\0' && v >= kMinLevel && v <= kMaxLevel) {
      level_.store(static_cast<int>(v), std::memory_order_relaxed);
      envLevelForced_ = true;
    }
  }
  const std::string pathText = envVar("TRINO_ODBC_LOG_PATH");
  if (!pathText.empty()) path_ = pathText;
  if (level_.load(std::memory_order_relaxed) > kMinLevel && !file_) openSinkLocked();
}

void Logger::openSinkLocked() {
  if (file_) return;
  std::string target = path_.empty() ? defaultLogDirectory() : path_;
  if (isDirectory(target)) {
    const char last = target.empty() ? '/' : target.back();
    if (last != '/' && last != '\\') {
#if defined(TRINO_ODBC_WINDOWS)
      target.push_back('\\');
#else
      target.push_back('/');
#endif
    }
    target += "trino_odbc_" + std::to_string(currentProcessId()) + ".log";
  }
  std::FILE* f = openAppend(target);
  if (!f) {
    // Nothing sensible can be written anywhere (no console in an ODBC host). Disable logging rather
    // than retrying the failing open on every call; a later configure() with a usable path re-enables.
    level_.store(kMinLevel, std::memory_order_relaxed);
    path_.clear();
    return;
  }
  file_ = f;
  path_ = target;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
  if (!enabled(level) || level == LogLevel::Off) return;
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  char header[96];
  const int hn = std::snprintf(header, sizeof(header), "%s [%llu] %-5s ", ts, currentThreadId(), levelName(level));
  std::string line;
  line.reserve(static_cast<size_t>(hn > 0 ? hn : 0) + component.size() + message.size() + 4);
  if (hn > 0) line.append(header, static_cast<size_t>(hn));
  line.append(component);
  line.append(": ");
  line.append(message);
  // Embedded newlines would forge fake log lines; keep each record on one physical line.
  for (char& c : line) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  line.push_back('\n');

  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) openSinkLocked();
  if (!file_) return;
  std::FILE* f = static_cast<std::FILE*>(file_);
  std::fwrite(line.data(), 1, line.size(), f);
  std::fflush(f);
}

void Logger::logf(LogLevel level, const char* component, const char* fmt, ...) {
  if (!enabled(level)) return;
  va_list args;
  va_start(args, fmt);
  vlogf(level, component, fmt, args);
  va_end(args);
}

void Logger::vlogf(LogLevel level, const char* component, const char* fmt, va_list args) {
  if (!enabled(level)) return;
  if (!fmt) fmt = "";
  va_list probe;
  va_copy(probe, args);
  char stackBuf[1024];
  const int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, probe);
  va_end(probe);
  if (n < 0) {
    log(level, component ? component : "", fmt);
    return;
  }
  if (static_cast<size_t>(n) < sizeof(stackBuf)) {
    log(level, component ? component : "", std::string_view(stackBuf, static_cast<size_t>(n)));
    return;
  }
  std::string big(static_cast<size_t>(n), '\0');
  va_list again;
  va_copy(again, args);
  std::vsnprintf(&big[0], static_cast<size_t>(n) + 1, fmt, again);
  va_end(again);
  log(level, component ? component : "", big);
}

}  // namespace trino_odbc
