#pragma once
// Process-wide, thread-safe file logger. Configured from DSN keys (LogLevel/LogPath) and from the
// TRINO_ODBC_LOG_LEVEL / TRINO_ODBC_LOG_PATH environment variables (environment wins).
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <string>
#include <string_view>

namespace trino_odbc {

enum class LogLevel : int { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Trace = 5 };

class Logger {
 public:
  static Logger& global();

  /// Raise the level and/or set the sink. The highest level requested by any live connection wins;
  /// the first non-empty path wins. `path` may be a directory (a file `trino_odbc_<pid>.log` is created
  /// inside) or a file path.
  void configure(int level, std::string_view path);
  /// Reads TRINO_ODBC_LOG_LEVEL / TRINO_ODBC_LOG_PATH. Safe to call many times (first call wins).
  void configureFromEnvironment();

  bool enabled(LogLevel level) const {
    return static_cast<int>(level) <= level_.load(std::memory_order_relaxed);
  }
  int level() const { return level_.load(std::memory_order_relaxed); }
  std::string path() const;

  /// Writes one line: `2026-09-27 12:00:00.123 [tid] LEVEL component: message`.
  void log(LogLevel level, std::string_view component, std::string_view message);
  /// printf-style variant. Arguments must be plain C types (use .c_str() for std::string).
  void logf(LogLevel level, const char* component, const char* fmt, ...);
  void vlogf(LogLevel level, const char* component, const char* fmt, va_list args);

  /// Returns a masked representation for secrets ("ab***yz" or "***").
  static std::string mask(std::string_view secret);
  static const char* levelName(LogLevel level);

 private:
  Logger() = default;
  void openSinkLocked();

  std::atomic<int> level_{0};
  mutable std::mutex mutex_;
  std::string path_;
  void* file_ = nullptr;  // FILE*
  bool envApplied_ = false;
  bool envLevelForced_ = false;
};

#define TRINO_LOG_ENABLED(lvl) (::trino_odbc::Logger::global().enabled(lvl))
#define TRINO_LOGF(lvl, comp, ...)                                          \
  do {                                                                      \
    if (TRINO_LOG_ENABLED(lvl)) ::trino_odbc::Logger::global().logf(lvl, comp, __VA_ARGS__); \
  } while (0)
#define TRINO_LOG_ERROR(comp, ...) TRINO_LOGF(::trino_odbc::LogLevel::Error, comp, __VA_ARGS__)
#define TRINO_LOG_WARN(comp, ...) TRINO_LOGF(::trino_odbc::LogLevel::Warn, comp, __VA_ARGS__)
#define TRINO_LOG_INFO(comp, ...) TRINO_LOGF(::trino_odbc::LogLevel::Info, comp, __VA_ARGS__)
#define TRINO_LOG_DEBUG(comp, ...) TRINO_LOGF(::trino_odbc::LogLevel::Debug, comp, __VA_ARGS__)
#define TRINO_LOG_TRACE(comp, ...) TRINO_LOGF(::trino_odbc::LogLevel::Trace, comp, __VA_ARGS__)

}  // namespace trino_odbc
