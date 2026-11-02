// Time zone database. Three backends share one "period" abstraction (a half-open UTC interval with a
// constant offset): TZif files (Unix and $TZDIR anywhere), ICU on Windows (the combined system
// icu.dll of Windows 10 1903+, which is also the driver's minimum), and the Windows registry rules as
// the last resort. Gap/overlap resolution for wall-clock input is done generically on top of periods,
// so every backend behaves identically. When no backend can resolve the OS zone, systemZoneName()
// returns "" and the driver omits X-Trino-Time-Zone (the server's zone applies) instead of sending a
// zone it cannot convert with.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#  define _WIN32_WINNT 0x0A00  // Windows 10 (DYNAMIC_TIME_ZONE_INFORMATION and friends)
#endif
#include "util/tz.h"

#include "platform.h"
#include "util/log.h"
#include "util/strings.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#if defined(TRINO_ODBC_UNIX)
#  include <climits>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#if defined(TRINO_ODBC_WINDOWS)
#  include <wchar.h>
#  ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#    define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#  endif
#endif

namespace trino_odbc {

namespace {

constexpr const char* kLog = "tz";
constexpr int64_t kMinTime = INT64_MIN;
constexpr int64_t kMaxTime = INT64_MAX;
// Inputs are clamped so that offset arithmetic (+-26h) and year computations can never overflow.
constexpr int64_t kClampSeconds = int64_t(1) << 50;
// POSIX limits zone offsets to +-24:59:59; the search window for wall-clock resolution covers it.
constexpr int64_t kOffsetWindow = 26 * 3600;
constexpr int64_t kSecondsPerDay = 86400;

// ---- Civil calendar helpers (proleptic Gregorian, Howard Hinnant's algorithms) ----------------

int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int64_t& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y += m <= 2;
}

bool isLeapYear(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int64_t y, int m) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && isLeapYear(y)) ? 29 : kDays[m - 1];
}

/// 0 = Sunday. 1970-01-01 was a Thursday.
int weekdayOfDays(int64_t days) {
  const int64_t r = ((days % 7) + 7 + 4) % 7;
  return static_cast<int>(r);
}

int64_t yearOfUtc(int64_t seconds) {
  int64_t days = seconds / kSecondsPerDay;
  if (seconds % kSecondsPerDay < 0) --days;
  int64_t y;
  int m, d;
  civilFromDays(days, y, m, d);
  return y;
}

int64_t clampTime(int64_t t) {
  if (t > kClampSeconds) return kClampSeconds;
  if (t < -kClampSeconds) return -kClampSeconds;
  return t;
}

// ---- Periods and rules --------------------------------------------------------------------

struct Period {
  int64_t start = kMinTime;  // inclusive UTC
  int64_t end = kMaxTime;    // exclusive UTC
  int32_t offset = 0;        // seconds east of UTC
};

struct RuleDate {
  enum class Kind { Julian1, Julian0, MonthWeekDay } kind = Kind::MonthWeekDay;
  int day = 0;       // Jn / n value, or day-of-week (0 = Sunday) for Mm.w.d
  int month = 1;     // Mm.w.d
  int week = 1;      // 1..5 (5 = last)
  int64_t time = 7200;  // seconds after local midnight; may be negative or > 24h (TZif v3)
};

struct PosixRule {
  int32_t stdOffset = 0;
  bool hasDst = false;
  int32_t dstOffset = 0;
  RuleDate start, end;
};

/// One year's worth of DST information, produced by a backend for a given year.
struct YearRule {
  bool hasDst = false;
  int64_t dstStartUtc = 0;
  int64_t dstEndUtc = 0;
  int32_t stdOffset = 0;
  int32_t dstOffset = 0;
};

/// Epoch seconds of the local wall-clock instant a rule date denotes in `year`.
int64_t ruleLocalSeconds(const RuleDate& r, int64_t year) {
  const int64_t jan1 = daysFromCivil(year, 1, 1);
  int64_t dayIndex = 0;
  switch (r.kind) {
    case RuleDate::Kind::Julian1:  // 1..365, February 29 never counted
      dayIndex = r.day - 1;
      if (isLeapYear(year) && r.day >= 60) dayIndex = r.day;
      break;
    case RuleDate::Kind::Julian0:  // 0..365, February 29 counted
      dayIndex = r.day;
      break;
    case RuleDate::Kind::MonthWeekDay: {
      const int64_t first = daysFromCivil(year, r.month, 1);
      const int firstWd = weekdayOfDays(first);
      int day = 1 + ((r.day - firstWd + 7) % 7) + (r.week - 1) * 7;
      const int dim = daysInMonth(year, r.month);
      while (day > dim) day -= 7;
      dayIndex = (first - jan1) + day - 1;
      break;
    }
  }
  return (jan1 + dayIndex) * kSecondsPerDay + r.time;
}

struct Transition {
  int64_t at;
  int32_t before;
  int32_t after;
};

/// Builds the period containing `utc` from per-year rules (POSIX footer or Windows registry). Three
/// years around the instant are enough because rules change at most twice per year.
template <typename YearRuleFn>
Period periodFromYearRules(int64_t utc, int32_t approxOffset, YearRuleFn yearRule) {
  const int64_t year = yearOfUtc(clampTime(utc) + approxOffset);
  std::vector<Transition> points;
  points.reserve(6);
  for (int64_t y = year - 1; y <= year + 1; ++y) {
    const YearRule r = yearRule(y);
    if (r.hasDst) {
      points.push_back({r.dstStartUtc, r.stdOffset, r.dstOffset});
      points.push_back({r.dstEndUtc, r.dstOffset, r.stdOffset});
    } else {
      // A DST-free year still needs a point so that the offset of that year is represented.
      points.push_back({daysFromCivil(y, 1, 1) * kSecondsPerDay - r.stdOffset, r.stdOffset, r.stdOffset});
    }
  }
  std::sort(points.begin(), points.end(), [](const Transition& a, const Transition& b) { return a.at < b.at; });
  Period p;
  size_t i = 0;
  while (i < points.size() && points[i].at <= utc) ++i;
  if (i == 0) {
    p.offset = points.front().before;
    p.end = points.front().at;
    return p;
  }
  p.start = points[i - 1].at;
  p.offset = points[i - 1].after;
  p.end = i < points.size() ? points[i].at : kMaxTime;
  return p;
}

Period rulePeriodAt(const PosixRule& rule, int64_t utc) {
  if (!rule.hasDst) {
    Period p;
    p.offset = rule.stdOffset;
    return p;
  }
  return periodFromYearRules(utc, rule.stdOffset, [&rule](int64_t y) {
    YearRule r;
    r.hasDst = true;
    r.stdOffset = rule.stdOffset;
    r.dstOffset = rule.dstOffset;
    // POSIX: the start time is expressed in standard local time, the end time in daylight local time.
    r.dstStartUtc = ruleLocalSeconds(rule.start, y) - rule.stdOffset;
    r.dstEndUtc = ruleLocalSeconds(rule.end, y) - rule.dstOffset;
    return r;
  });
}

// ---- POSIX TZ string parser ---------------------------------------------------------------

struct PosixCursor {
  std::string_view s;
  size_t pos = 0;
  bool done() const { return pos >= s.size(); }
  char peek() const { return done() ? '\0' : s[pos]; }
};

bool parsePosixName(PosixCursor& c) {
  if (c.peek() == '<') {
    const size_t close = c.s.find('>', c.pos + 1);
    if (close == std::string_view::npos || close == c.pos + 1) return false;
    for (size_t i = c.pos + 1; i < close; ++i) {
      const char ch = c.s[i];
      const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                      ch == '+' || ch == '-';
      if (!ok) return false;
    }
    c.pos = close + 1;
    return true;
  }
  size_t n = 0;
  while (!c.done() && ((c.peek() >= 'A' && c.peek() <= 'Z') || (c.peek() >= 'a' && c.peek() <= 'z'))) {
    ++c.pos;
    ++n;
  }
  return n >= 3;
}

/// [+-]hh[:mm[:ss]]; `maxHours` bounds the hour field (24 for offsets, 167 for TZif v3 rule times).
bool parsePosixTime(PosixCursor& c, int maxHours, int64_t* out) {
  bool negative = false;
  if (c.peek() == '+' || c.peek() == '-') {
    negative = c.peek() == '-';
    ++c.pos;
  }
  int parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    int digits = 0;
    int v = 0;
    while (!c.done() && c.peek() >= '0' && c.peek() <= '9' && digits < 3) {
      v = v * 10 + (c.peek() - '0');
      ++c.pos;
      ++digits;
    }
    if (digits == 0) return false;
    parts[i] = v;
    if (c.peek() != ':') break;
    ++c.pos;
    if (i == 2) return false;
  }
  if (parts[0] > maxHours || parts[1] > 59 || parts[2] > 59) return false;
  int64_t total = parts[0] * 3600LL + parts[1] * 60LL + parts[2];
  if (out) *out = negative ? -total : total;
  return true;
}

bool parsePosixDate(PosixCursor& c, RuleDate& out) {
  if (c.peek() == 'J') {
    ++c.pos;
    int v = 0, digits = 0;
    while (!c.done() && c.peek() >= '0' && c.peek() <= '9' && digits < 3) { v = v * 10 + (c.peek() - '0'); ++c.pos; ++digits; }
    if (digits == 0 || v < 1 || v > 365) return false;
    out.kind = RuleDate::Kind::Julian1;
    out.day = v;
  } else if (c.peek() == 'M') {
    ++c.pos;
    int fields[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      int v = 0, digits = 0;
      while (!c.done() && c.peek() >= '0' && c.peek() <= '9' && digits < 2) { v = v * 10 + (c.peek() - '0'); ++c.pos; ++digits; }
      if (digits == 0) return false;
      fields[i] = v;
      if (i < 2) {
        if (c.peek() != '.') return false;
        ++c.pos;
      }
    }
    if (fields[0] < 1 || fields[0] > 12 || fields[1] < 1 || fields[1] > 5 || fields[2] < 0 || fields[2] > 6) return false;
    out.kind = RuleDate::Kind::MonthWeekDay;
    out.month = fields[0];
    out.week = fields[1];
    out.day = fields[2];
  } else {
    int v = 0, digits = 0;
    while (!c.done() && c.peek() >= '0' && c.peek() <= '9' && digits < 3) { v = v * 10 + (c.peek() - '0'); ++c.pos; ++digits; }
    if (digits == 0 || v > 365) return false;
    out.kind = RuleDate::Kind::Julian0;
    out.day = v;
  }
  out.time = 7200;
  if (c.peek() == '/') {
    ++c.pos;
    if (!parsePosixTime(c, 167, &out.time)) return false;
  }
  return true;
}

/// Parses "std offset[dst[offset][,start[/time],end[/time]]]" (POSIX.1 + TZif v3 extensions).
bool parsePosixTz(std::string_view text, PosixRule& out) {
  PosixCursor c{text, 0};
  PosixRule r;
  if (!parsePosixName(c)) return false;
  int64_t off = 0;
  if (!parsePosixTime(c, 24, &off)) return false;
  r.stdOffset = static_cast<int32_t>(-off);  // POSIX counts hours *west* of Greenwich
  if (c.done()) {
    out = r;
    return true;
  }
  if (!parsePosixName(c)) return false;
  r.hasDst = true;
  r.dstOffset = r.stdOffset + 3600;
  if (c.peek() == '+' || c.peek() == '-' || (c.peek() >= '0' && c.peek() <= '9')) {
    if (!parsePosixTime(c, 24, &off)) return false;
    r.dstOffset = static_cast<int32_t>(-off);
  }
  if (c.done()) {
    // No rule given: POSIX leaves it implementation-defined; glibc and zic use the current US rules.
    r.start.kind = RuleDate::Kind::MonthWeekDay;
    r.start.month = 3; r.start.week = 2; r.start.day = 0; r.start.time = 7200;
    r.end.kind = RuleDate::Kind::MonthWeekDay;
    r.end.month = 11; r.end.week = 1; r.end.day = 0; r.end.time = 7200;
    out = r;
    return true;
  }
  if (c.peek() != ',') return false;
  ++c.pos;
  if (!parsePosixDate(c, r.start)) return false;
  if (c.peek() != ',') return false;
  ++c.pos;
  if (!parsePosixDate(c, r.end)) return false;
  if (!c.done()) return false;
  out = r;
  return true;
}

// ---- Zone representation ---------------------------------------------------------------------

struct Zone {
  enum class Backend { Fixed, Table, Rule, Icu, WinRule };
  Backend backend = Backend::Fixed;
  int32_t fixedOffset = 0;
  // Table (TZif)
  std::vector<int64_t> transitions;   // UTC instants, non-decreasing
  std::vector<uint8_t> typeIndex;     // one per transition
  std::vector<int32_t> typeOffsets;   // utoff per local time type
  bool hasRule = false;
  PosixRule rule;                     // footer rule (Table) or the whole zone (Rule)
#if defined(TRINO_ODBC_WINDOWS)
  std::mutex nativeMutex;             // ICU calendars are not thread-safe
  void* ucal = nullptr;
  DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
#endif
  ~Zone();
};

Period tablePeriodAt(const Zone& z, int64_t utc) {
  Period p;
  if (z.transitions.empty()) {
    if (z.hasRule) return rulePeriodAt(z.rule, utc);
    p.offset = z.typeOffsets.empty() ? 0 : z.typeOffsets[0];
    return p;
  }
  if (utc < z.transitions.front()) {
    // RFC 8536: instants before the first transition use time type 0.
    p.end = z.transitions.front();
    p.offset = z.typeOffsets[0];
    return p;
  }
  const auto it = std::upper_bound(z.transitions.begin(), z.transitions.end(), utc);
  const size_t i = static_cast<size_t>(it - z.transitions.begin()) - 1;
  if (i + 1 == z.transitions.size()) {
    if (z.hasRule) {
      Period r = rulePeriodAt(z.rule, utc);
      if (r.start < z.transitions.back()) r.start = z.transitions.back();
      return r;
    }
    p.start = z.transitions.back();
    p.offset = z.typeOffsets[z.typeIndex.back()];
    return p;
  }
  p.start = z.transitions[i];
  p.end = z.transitions[i + 1];
  p.offset = z.typeOffsets[z.typeIndex[i]];
  return p;
}

// ---- TZif parser ---------------------------------------------------------------------------

uint32_t readU32(const unsigned char* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
int64_t readS32(const unsigned char* p) { return static_cast<int32_t>(readU32(p)); }
int64_t readS64(const unsigned char* p) {
  return static_cast<int64_t>((uint64_t(readU32(p)) << 32) | uint64_t(readU32(p + 4)));
}

struct TzifHeader {
  char version;
  uint32_t isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt;
};

bool readTzifHeader(const unsigned char* p, size_t avail, TzifHeader& h) {
  if (avail < 44 || std::memcmp(p, "TZif", 4) != 0) return false;
  h.version = static_cast<char>(p[4]);
  h.isutcnt = readU32(p + 20);
  h.isstdcnt = readU32(p + 24);
  h.leapcnt = readU32(p + 28);
  h.timecnt = readU32(p + 32);
  h.typecnt = readU32(p + 36);
  h.charcnt = readU32(p + 40);
  // Sanity bounds keep the size arithmetic below far away from overflow on hostile files.
  constexpr uint32_t kLimit = 1u << 24;
  return h.typecnt >= 1 && h.typecnt <= 256 && h.timecnt <= kLimit && h.charcnt <= kLimit && h.leapcnt <= kLimit &&
         h.isstdcnt <= kLimit && h.isutcnt <= kLimit;
}

uint64_t tzifBlockSize(const TzifHeader& h, size_t timeSize) {
  return uint64_t(h.timecnt) * (timeSize + 1) + uint64_t(h.typecnt) * 6 + h.charcnt +
         uint64_t(h.leapcnt) * (timeSize + 4) + h.isstdcnt + h.isutcnt;
}

bool parseTzifBlock(const unsigned char* p, const TzifHeader& h, size_t timeSize, Zone& z) {
  z.transitions.clear();
  z.typeIndex.clear();
  z.typeOffsets.clear();
  z.transitions.reserve(h.timecnt);
  z.typeIndex.reserve(h.timecnt);
  size_t pos = 0;
  int64_t last = kMinTime;
  for (uint32_t i = 0; i < h.timecnt; ++i) {
    const int64_t t = timeSize == 8 ? readS64(p + pos) : readS32(p + pos);
    pos += timeSize;
    if (t < last) return false;  // must be sorted
    last = t;
    z.transitions.push_back(t);
  }
  for (uint32_t i = 0; i < h.timecnt; ++i) {
    const uint8_t idx = p[pos++];
    if (idx >= h.typecnt) return false;
    z.typeIndex.push_back(idx);
  }
  z.typeOffsets.reserve(h.typecnt);
  for (uint32_t i = 0; i < h.typecnt; ++i) {
    const int64_t utoff = readS32(p + pos);
    pos += 6;  // utoff(4) isdst(1) desigidx(1)
    if (utoff <= -kSecondsPerDay * 2 || utoff >= kSecondsPerDay * 2) return false;
    z.typeOffsets.push_back(static_cast<int32_t>(utoff));
  }
  // Designations, leap seconds and the isstd/isut flags do not affect UTC offsets.
  return true;
}

bool parseTzif(const std::string& data, Zone& z) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
  const size_t size = data.size();
  TzifHeader h1;
  if (!readTzifHeader(p, size, h1)) return false;
  const uint64_t v1Size = tzifBlockSize(h1, 4);
  if (44 + v1Size > size) return false;
  z.backend = Zone::Backend::Table;
  z.hasRule = false;
  if (h1.version < '2') {
    return parseTzifBlock(p + 44, h1, 4, z);
  }
  const size_t off2 = static_cast<size_t>(44 + v1Size);
  TzifHeader h2;
  if (!readTzifHeader(p + off2, size - off2, h2)) return false;
  const uint64_t v2Size = tzifBlockSize(h2, 8);
  if (off2 + 44 + v2Size > size) return false;
  if (!parseTzifBlock(p + off2 + 44, h2, 8, z)) return false;
  const size_t footer = static_cast<size_t>(off2 + 44 + v2Size);
  if (footer < size && p[footer] == '\n') {
    const size_t end = data.find('\n', footer + 1);
    if (end != std::string::npos && end > footer + 1) {
      const std::string_view tz(data.data() + footer + 1, end - footer - 1);
      PosixRule rule;
      if (parsePosixTz(tz, rule)) {
        z.rule = rule;
        z.hasRule = true;
      } else {
        TRINO_LOG_DEBUG(kLog, "ignoring unparsable TZif footer '%.*s'", static_cast<int>(tz.size()), tz.data());
      }
    }
  }
  return true;
}

// ---- Zone name handling -----------------------------------------------------------------------

/// Guards the file lookup: only IANA-shaped names may be joined to a zoneinfo directory.
bool validZoneName(std::string_view name) {
  if (name.empty() || name.size() > 128 || name.front() == '/' || name.front() == '.') return false;
  for (const char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                    c == '_' || c == '+' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return name.find("..") == std::string_view::npos && name.find("//") == std::string_view::npos &&
         name.back() != '/';
}

/// Names that must resolve without any database: UTC aliases and Etc/GMT+-N.
bool builtinOffset(std::string_view name, int32_t* offset) {
  static constexpr std::string_view kUtcNames[] = {
      "UTC", "Z", "GMT", "UT", "UCT", "Zulu", "Universal", "Greenwich", "Etc/UTC", "Etc/GMT", "Etc/UCT",
      "Etc/Zulu", "Etc/Universal", "Etc/Greenwich", "GMT0", "GMT+0", "GMT-0", "Etc/GMT0", "Etc/GMT+0", "Etc/GMT-0"};
  for (const auto& n : kUtcNames) {
    if (iequals(name, n)) {
      *offset = 0;
      return true;
    }
  }
  if (istartsWith(name, "Etc/GMT") && name.size() >= 9 && name.size() <= 10) {
    // Etc/GMT+5 is UTC-5: the sign follows the POSIX (west-positive) convention.
    const char sign = name[7];
    if (sign != '+' && sign != '-') return false;
    int hours = 0;
    for (size_t i = 8; i < name.size(); ++i) {
      if (name[i] < '0' || name[i] > '9') return false;
      hours = hours * 10 + (name[i] - '0');
    }
    if (hours > 14) return false;
    *offset = (sign == '+' ? -hours : hours) * 3600;
    return true;
  }
  return parseFixedOffset(name, offset);
}

std::string envValue(const char* name) {
#if defined(TRINO_ODBC_WINDOWS)
  char buf[1024];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return std::string();
  return std::string(buf, n);
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

std::vector<std::string> zoneDirectories() {
  std::vector<std::string> dirs;
  const std::string tzdir = envValue("TZDIR");
  if (!tzdir.empty()) dirs.push_back(tzdir);
#if defined(TRINO_ODBC_UNIX)
  dirs.push_back("/usr/share/zoneinfo");
  dirs.push_back("/var/db/timezone/zoneinfo");
  dirs.push_back("/usr/lib/zoneinfo");
  dirs.push_back("/usr/share/lib/zoneinfo");
#endif
  return dirs;
}

bool readSmallFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  constexpr std::streamsize kMaxSize = 4 * 1024 * 1024;
  in.seekg(0, std::ios::end);
  const std::streamsize size = in.tellg();
  if (size < 0 || size > kMaxSize) return false;
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (size > 0 && !in.read(&out[0], size)) return false;
  return true;
}

bool loadZoneFile(std::string_view name, Zone& z) {
  if (!validZoneName(name)) return false;
  std::string data;
  for (const std::string& dir : zoneDirectories()) {
    std::string path = dir;
    if (path.back() != '/' && path.back() != '\\') path.push_back('/');
    path.append(name);
    if (!readSmallFile(path, data)) continue;
    if (parseTzif(data, z)) return true;
    TRINO_LOG_DEBUG(kLog, "%s is not a valid TZif file", path.c_str());
  }
  return false;
}

// ---- Windows: ICU and registry backends ------------------------------------------------------

#if defined(TRINO_ODBC_WINDOWS)
struct WinZoneMap {
  const char* windows;
  const char* iana;
};

// CLDR windowsZones.xml territory "001" mappings, followed by common territory-specific aliases so
// the reverse lookup (IANA -> Windows) accepts the names people actually put in DSNs.
constexpr WinZoneMap kWindowsZones[] = {
    {"Dateline Standard Time", "Etc/GMT+12"},
    {"UTC-11", "Etc/GMT+11"},
    {"Aleutian Standard Time", "America/Adak"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"},
    {"Marquesas Standard Time", "Pacific/Marquesas"},
    {"Alaskan Standard Time", "America/Anchorage"},
    {"UTC-09", "Etc/GMT+9"},
    {"Pacific Standard Time (Mexico)", "America/Tijuana"},
    {"UTC-08", "Etc/GMT+8"},
    {"Pacific Standard Time", "America/Los_Angeles"},
    {"US Mountain Standard Time", "America/Phoenix"},
    {"Mountain Standard Time (Mexico)", "America/Mazatlan"},
    {"Mountain Standard Time", "America/Denver"},
    {"Yukon Standard Time", "America/Whitehorse"},
    {"Central America Standard Time", "America/Guatemala"},
    {"Central Standard Time", "America/Chicago"},
    {"Easter Island Standard Time", "Pacific/Easter"},
    {"Central Standard Time (Mexico)", "America/Mexico_City"},
    {"Canada Central Standard Time", "America/Regina"},
    {"SA Pacific Standard Time", "America/Bogota"},
    {"Eastern Standard Time (Mexico)", "America/Cancun"},
    {"Eastern Standard Time", "America/New_York"},
    {"Haiti Standard Time", "America/Port-au-Prince"},
    {"Cuba Standard Time", "America/Havana"},
    {"US Eastern Standard Time", "America/Indianapolis"},
    {"Turks And Caicos Standard Time", "America/Grand_Turk"},
    {"Paraguay Standard Time", "America/Asuncion"},
    {"Atlantic Standard Time", "America/Halifax"},
    {"Venezuela Standard Time", "America/Caracas"},
    {"Central Brazilian Standard Time", "America/Cuiaba"},
    {"SA Western Standard Time", "America/La_Paz"},
    {"Pacific SA Standard Time", "America/Santiago"},
    {"Newfoundland Standard Time", "America/St_Johns"},
    {"Tocantins Standard Time", "America/Araguaina"},
    {"E. South America Standard Time", "America/Sao_Paulo"},
    {"SA Eastern Standard Time", "America/Cayenne"},
    {"Argentina Standard Time", "America/Buenos_Aires"},
    {"Montevideo Standard Time", "America/Montevideo"},
    {"Magallanes Standard Time", "America/Punta_Arenas"},
    {"Saint Pierre Standard Time", "America/Miquelon"},
    {"Bahia Standard Time", "America/Bahia"},
    {"UTC-02", "Etc/GMT+2"},
    {"Mid-Atlantic Standard Time", "Etc/GMT+2"},
    {"Greenland Standard Time", "America/Godthab"},
    {"Azores Standard Time", "Atlantic/Azores"},
    {"Cape Verde Standard Time", "Atlantic/Cape_Verde"},
    {"UTC", "Etc/UTC"},
    {"Coordinated Universal Time", "Etc/UTC"},
    {"GMT Standard Time", "Europe/London"},
    {"Greenwich Standard Time", "Atlantic/Reykjavik"},
    {"Sao Tome Standard Time", "Africa/Sao_Tome"},
    {"Morocco Standard Time", "Africa/Casablanca"},
    {"W. Europe Standard Time", "Europe/Berlin"},
    {"Central Europe Standard Time", "Europe/Budapest"},
    {"Romance Standard Time", "Europe/Paris"},
    {"Central European Standard Time", "Europe/Warsaw"},
    {"W. Central Africa Standard Time", "Africa/Lagos"},
    {"GTB Standard Time", "Europe/Bucharest"},
    {"Middle East Standard Time", "Asia/Beirut"},
    {"Egypt Standard Time", "Africa/Cairo"},
    {"E. Europe Standard Time", "Europe/Chisinau"},
    {"West Bank Standard Time", "Asia/Hebron"},
    {"South Africa Standard Time", "Africa/Johannesburg"},
    {"FLE Standard Time", "Europe/Kiev"},
    {"Israel Standard Time", "Asia/Jerusalem"},
    {"South Sudan Standard Time", "Africa/Juba"},
    {"Kaliningrad Standard Time", "Europe/Kaliningrad"},
    {"Sudan Standard Time", "Africa/Khartoum"},
    {"Libya Standard Time", "Africa/Tripoli"},
    {"Namibia Standard Time", "Africa/Windhoek"},
    {"Jordan Standard Time", "Asia/Amman"},
    {"Arabic Standard Time", "Asia/Baghdad"},
    {"Syria Standard Time", "Asia/Damascus"},
    {"Turkey Standard Time", "Europe/Istanbul"},
    {"Arab Standard Time", "Asia/Riyadh"},
    {"Belarus Standard Time", "Europe/Minsk"},
    {"Russian Standard Time", "Europe/Moscow"},
    {"E. Africa Standard Time", "Africa/Nairobi"},
    {"Volgograd Standard Time", "Europe/Volgograd"},
    {"Iran Standard Time", "Asia/Tehran"},
    {"Arabian Standard Time", "Asia/Dubai"},
    {"Astrakhan Standard Time", "Europe/Astrakhan"},
    {"Azerbaijan Standard Time", "Asia/Baku"},
    {"Russia Time Zone 3", "Europe/Samara"},
    {"Mauritius Standard Time", "Indian/Mauritius"},
    {"Saratov Standard Time", "Europe/Saratov"},
    {"Georgian Standard Time", "Asia/Tbilisi"},
    {"Caucasus Standard Time", "Asia/Yerevan"},
    {"Afghanistan Standard Time", "Asia/Kabul"},
    {"West Asia Standard Time", "Asia/Tashkent"},
    {"Ekaterinburg Standard Time", "Asia/Yekaterinburg"},
    {"Pakistan Standard Time", "Asia/Karachi"},
    {"Qyzylorda Standard Time", "Asia/Qyzylorda"},
    {"India Standard Time", "Asia/Calcutta"},
    {"Sri Lanka Standard Time", "Asia/Colombo"},
    {"Nepal Standard Time", "Asia/Katmandu"},
    {"Central Asia Standard Time", "Asia/Bishkek"},
    {"Bangladesh Standard Time", "Asia/Dhaka"},
    {"Omsk Standard Time", "Asia/Omsk"},
    {"Myanmar Standard Time", "Asia/Rangoon"},
    {"SE Asia Standard Time", "Asia/Bangkok"},
    {"Altai Standard Time", "Asia/Barnaul"},
    {"W. Mongolia Standard Time", "Asia/Hovd"},
    {"North Asia Standard Time", "Asia/Krasnoyarsk"},
    {"N. Central Asia Standard Time", "Asia/Novosibirsk"},
    {"Tomsk Standard Time", "Asia/Tomsk"},
    {"China Standard Time", "Asia/Shanghai"},
    {"North Asia East Standard Time", "Asia/Irkutsk"},
    {"Singapore Standard Time", "Asia/Singapore"},
    {"W. Australia Standard Time", "Australia/Perth"},
    {"Taipei Standard Time", "Asia/Taipei"},
    {"Ulaanbaatar Standard Time", "Asia/Ulaanbaatar"},
    {"Aus Central W. Standard Time", "Australia/Eucla"},
    {"Transbaikal Standard Time", "Asia/Chita"},
    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"North Korea Standard Time", "Asia/Pyongyang"},
    {"Korea Standard Time", "Asia/Seoul"},
    {"Yakutsk Standard Time", "Asia/Yakutsk"},
    {"Cen. Australia Standard Time", "Australia/Adelaide"},
    {"AUS Central Standard Time", "Australia/Darwin"},
    {"E. Australia Standard Time", "Australia/Brisbane"},
    {"AUS Eastern Standard Time", "Australia/Sydney"},
    {"West Pacific Standard Time", "Pacific/Port_Moresby"},
    {"Tasmania Standard Time", "Australia/Hobart"},
    {"Vladivostok Standard Time", "Asia/Vladivostok"},
    {"Lord Howe Standard Time", "Australia/Lord_Howe"},
    {"Bougainville Standard Time", "Pacific/Bougainville"},
    {"Russia Time Zone 10", "Asia/Srednekolymsk"},
    {"Magadan Standard Time", "Asia/Magadan"},
    {"Norfolk Standard Time", "Pacific/Norfolk"},
    {"Sakhalin Standard Time", "Asia/Sakhalin"},
    {"Central Pacific Standard Time", "Pacific/Guadalcanal"},
    {"Russia Time Zone 11", "Asia/Kamchatka"},
    {"Kamchatka Standard Time", "Asia/Kamchatka"},
    {"New Zealand Standard Time", "Pacific/Auckland"},
    {"UTC+12", "Etc/GMT-12"},
    {"Fiji Standard Time", "Pacific/Fiji"},
    {"Chatham Islands Standard Time", "Pacific/Chatham"},
    {"UTC+13", "Etc/GMT-13"},
    {"Tonga Standard Time", "Pacific/Tongatapu"},
    {"Samoa Standard Time", "Pacific/Apia"},
    {"Line Islands Standard Time", "Pacific/Kiritimati"},
    // ---- aliases and territory-specific zones (reverse lookup only) ----
    {"India Standard Time", "Asia/Kolkata"},
    {"Nepal Standard Time", "Asia/Kathmandu"},
    {"Myanmar Standard Time", "Asia/Yangon"},
    {"FLE Standard Time", "Europe/Kyiv"},
    {"FLE Standard Time", "Europe/Helsinki"},
    {"FLE Standard Time", "Europe/Sofia"},
    {"FLE Standard Time", "Europe/Riga"},
    {"FLE Standard Time", "Europe/Tallinn"},
    {"FLE Standard Time", "Europe/Vilnius"},
    {"Argentina Standard Time", "America/Argentina/Buenos_Aires"},
    {"Argentina Standard Time", "America/Argentina/Cordoba"},
    {"Greenland Standard Time", "America/Nuuk"},
    {"US Eastern Standard Time", "America/Indiana/Indianapolis"},
    {"SE Asia Standard Time", "Asia/Saigon"},
    {"SE Asia Standard Time", "Asia/Ho_Chi_Minh"},
    {"SE Asia Standard Time", "Asia/Jakarta"},
    {"SE Asia Standard Time", "Asia/Phnom_Penh"},
    {"SE Asia Standard Time", "Asia/Vientiane"},
    {"W. Europe Standard Time", "Europe/Amsterdam"},
    {"W. Europe Standard Time", "Europe/Rome"},
    {"W. Europe Standard Time", "Europe/Vienna"},
    {"W. Europe Standard Time", "Europe/Zurich"},
    {"W. Europe Standard Time", "Europe/Stockholm"},
    {"W. Europe Standard Time", "Europe/Oslo"},
    {"W. Europe Standard Time", "Europe/Luxembourg"},
    {"W. Europe Standard Time", "Europe/Monaco"},
    {"W. Europe Standard Time", "Europe/Malta"},
    {"Romance Standard Time", "Europe/Madrid"},
    {"Romance Standard Time", "Europe/Brussels"},
    {"Romance Standard Time", "Europe/Copenhagen"},
    {"Central Europe Standard Time", "Europe/Prague"},
    {"Central Europe Standard Time", "Europe/Belgrade"},
    {"Central Europe Standard Time", "Europe/Bratislava"},
    {"Central Europe Standard Time", "Europe/Ljubljana"},
    {"Central Europe Standard Time", "Europe/Tirane"},
    {"Central European Standard Time", "Europe/Zagreb"},
    {"Central European Standard Time", "Europe/Sarajevo"},
    {"Central European Standard Time", "Europe/Skopje"},
    {"GMT Standard Time", "Europe/Dublin"},
    {"GMT Standard Time", "Europe/Lisbon"},
    {"GMT Standard Time", "Atlantic/Canary"},
    {"GMT Standard Time", "Atlantic/Faroe"},
    {"GTB Standard Time", "Europe/Athens"},
    {"Eastern Standard Time", "America/Toronto"},
    {"Eastern Standard Time", "America/Detroit"},
    {"Eastern Standard Time", "America/Montreal"},
    {"Eastern Standard Time", "America/Nassau"},
    {"Pacific Standard Time", "America/Vancouver"},
    {"Mountain Standard Time", "America/Edmonton"},
    {"Mountain Standard Time", "America/Boise"},
    {"Central Standard Time", "America/Winnipeg"},
    {"Central Standard Time", "America/Matamoros"},
    {"Central Standard Time (Mexico)", "America/Monterrey"},
    {"Central Standard Time (Mexico)", "America/Merida"},
    {"Central America Standard Time", "America/Costa_Rica"},
    {"Central America Standard Time", "America/El_Salvador"},
    {"Central America Standard Time", "America/Managua"},
    {"Central America Standard Time", "America/Tegucigalpa"},
    {"Central America Standard Time", "America/Belize"},
    {"SA Pacific Standard Time", "America/Lima"},
    {"SA Pacific Standard Time", "America/Panama"},
    {"SA Pacific Standard Time", "America/Jamaica"},
    {"SA Pacific Standard Time", "America/Guayaquil"},
    {"SA Pacific Standard Time", "America/Cayman"},
    {"SA Western Standard Time", "America/Puerto_Rico"},
    {"SA Western Standard Time", "America/Santo_Domingo"},
    {"SA Western Standard Time", "America/Manaus"},
    {"SA Western Standard Time", "America/Barbados"},
    {"SA Western Standard Time", "America/Martinique"},
    {"SA Eastern Standard Time", "America/Fortaleza"},
    {"SA Eastern Standard Time", "America/Recife"},
    {"SA Eastern Standard Time", "America/Belem"},
    {"E. Africa Standard Time", "Africa/Addis_Ababa"},
    {"E. Africa Standard Time", "Africa/Dar_es_Salaam"},
    {"E. Africa Standard Time", "Africa/Kampala"},
    {"E. Africa Standard Time", "Indian/Antananarivo"},
    {"South Africa Standard Time", "Africa/Harare"},
    {"South Africa Standard Time", "Africa/Maputo"},
    {"South Africa Standard Time", "Africa/Lusaka"},
    {"South Africa Standard Time", "Africa/Gaborone"},
    {"South Africa Standard Time", "Africa/Maseru"},
    {"W. Central Africa Standard Time", "Africa/Algiers"},
    {"W. Central Africa Standard Time", "Africa/Tunis"},
    {"W. Central Africa Standard Time", "Africa/Kinshasa"},
    {"W. Central Africa Standard Time", "Africa/Luanda"},
    {"W. Central Africa Standard Time", "Africa/Douala"},
    {"Greenwich Standard Time", "Africa/Accra"},
    {"Greenwich Standard Time", "Africa/Abidjan"},
    {"Greenwich Standard Time", "Africa/Dakar"},
    {"Arab Standard Time", "Asia/Kuwait"},
    {"Arab Standard Time", "Asia/Qatar"},
    {"Arab Standard Time", "Asia/Bahrain"},
    {"Arab Standard Time", "Asia/Aden"},
    {"Arabian Standard Time", "Asia/Muscat"},
    {"Arabian Standard Time", "Indian/Reunion"},
    {"Arabian Standard Time", "Indian/Mahe"},
    {"West Asia Standard Time", "Asia/Almaty"},
    {"West Asia Standard Time", "Indian/Maldives"},
    {"West Asia Standard Time", "Asia/Ashgabat"},
    {"West Asia Standard Time", "Asia/Dushanbe"},
    {"China Standard Time", "Asia/Hong_Kong"},
    {"China Standard Time", "Asia/Macau"},
    {"Singapore Standard Time", "Asia/Kuala_Lumpur"},
    {"Singapore Standard Time", "Asia/Manila"},
    {"Singapore Standard Time", "Asia/Makassar"},
    {"Singapore Standard Time", "Asia/Brunei"},
    {"Tokyo Standard Time", "Asia/Jayapura"},
    {"Tokyo Standard Time", "Pacific/Palau"},
    {"N. Central Asia Standard Time", "Asia/Novokuznetsk"},
    {"AUS Eastern Standard Time", "Australia/Melbourne"},
    {"AUS Eastern Standard Time", "Australia/Canberra"},
    {"West Pacific Standard Time", "Pacific/Guam"},
    {"West Pacific Standard Time", "Pacific/Saipan"},
    {"West Pacific Standard Time", "Antarctica/DumontDUrville"},
    {"UTC+12", "Pacific/Majuro"},
    {"UTC+12", "Pacific/Tarawa"},
    {"UTC+12", "Pacific/Funafuti"},
    {"UTC+12", "Pacific/Wake"},
    {"UTC+12", "Pacific/Wallis"},
    {"Central Pacific Standard Time", "Pacific/Noumea"},
    {"Central Pacific Standard Time", "Pacific/Efate"},
    {"Hawaiian Standard Time", "Pacific/Tahiti"},
    {"Hawaiian Standard Time", "Pacific/Rarotonga"},
    {"UTC-11", "Pacific/Pago_Pago"},
    {"UTC-11", "Pacific/Midway"},
    {"UTC-11", "Pacific/Niue"},
    {"Atlantic Standard Time", "Atlantic/Bermuda"},
    {"Atlantic Standard Time", "America/Glace_Bay"},
    {"Atlantic Standard Time", "America/Moncton"},
    {"Atlantic Standard Time", "America/Thule"},
    {"Cape Verde Standard Time", "Etc/GMT+1"},
    {"Greenwich Standard Time", "Etc/GMT"},
    {"UTC", "UTC"},
    {"UTC", "Etc/GMT"},
    {"UTC", "GMT"},
};

std::string utf16ToUtf8Local(const wchar_t* s, int len) {
  if (!s || len == 0) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
  if (len < 0 && !out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

std::wstring utf8ToUtf16Local(std::string_view s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
  return out;
}

const char* windowsToIana(std::string_view windowsName) {
  for (const auto& e : kWindowsZones) {
    if (iequals(windowsName, e.windows)) return e.iana;
  }
  return nullptr;
}

const char* ianaToWindows(std::string_view iana) {
  for (const auto& e : kWindowsZones) {
    if (iequals(iana, e.iana)) return e.windows;
  }
  return nullptr;
}

// ICU C API surface, resolved at runtime from icu.dll (Windows 10 1703+). Symbol names are
// unversioned there, unlike the icuuc/icuin DLLs shipped with applications.
using UChar = uint16_t;
using UErrorCode = int;
using UBool = int8_t;
using UDate = double;
constexpr int kUcalGregorian = 1;
constexpr int kUcalZoneOffset = 15;
constexpr int kUcalDstOffset = 16;
constexpr int kUcalTransitionNext = 0;
constexpr int kUcalTransitionPreviousInclusive = 3;
// ICU's calendar supports roughly +-10^8 days; stay well inside it.
constexpr int64_t kIcuClampSeconds = int64_t(100000) * 365 * kSecondsPerDay;

struct IcuApi {
  HMODULE lib = nullptr;
  void* (*open)(const UChar*, int32_t, const char*, int, UErrorCode*) = nullptr;
  void (*close)(void*) = nullptr;
  void (*setMillis)(void*, UDate, UErrorCode*) = nullptr;
  int32_t (*get)(const void*, int, UErrorCode*) = nullptr;
  UBool (*transitionDate)(const void*, int, UDate*, UErrorCode*) = nullptr;
  int32_t (*canonicalId)(const UChar*, int32_t, UChar*, int32_t, UBool*, UErrorCode*) = nullptr;
  int32_t (*idForWindowsId)(const UChar*, int32_t, const char*, UChar*, int32_t, UErrorCode*) = nullptr;
  bool available() const { return open && close && setMillis && get && transitionDate && canonicalId; }
};

template <typename Fn>
void loadSymbol(HMODULE lib, const char* name, Fn& out) {
  out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, name)));
}

const IcuApi& icu() {
  static const IcuApi api = [] {
    IcuApi a;
    // System32 only: icu.dll is not a KnownDLL, so a bare-name load would also search the host's
    // current directory and PATH, where a planted icu.dll would run inside the ODBC host.
    a.lib = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!a.lib) {
      TRINO_LOG_INFO(kLog, "icu.dll not available; using registry time zone rules");
      return a;
    }
    loadSymbol(a.lib, "ucal_open", a.open);
    loadSymbol(a.lib, "ucal_close", a.close);
    loadSymbol(a.lib, "ucal_setMillis", a.setMillis);
    loadSymbol(a.lib, "ucal_get", a.get);
    loadSymbol(a.lib, "ucal_getTimeZoneTransitionDate", a.transitionDate);
    loadSymbol(a.lib, "ucal_getCanonicalTimeZoneID", a.canonicalId);
    loadSymbol(a.lib, "ucal_getTimeZoneIDForWindowsID", a.idForWindowsId);
    if (!a.available()) TRINO_LOG_INFO(kLog, "icu.dll lacks the calendar API; using registry rules");
    return a;
  }();
  return api;
}

std::vector<UChar> toUChars(std::string_view utf8) {
  const std::wstring w = utf8ToUtf16Local(utf8);
  return std::vector<UChar>(w.begin(), w.end());
}

bool openIcuZone(std::string_view name, Zone& z) {
  const IcuApi& api = icu();
  if (!api.available()) return false;
  const std::vector<UChar> id = toUChars(name);
  // ucal_open silently falls back to GMT for unknown ids; the canonical-id probe tells them apart.
  UChar canonical[128];
  UBool isSystem = 0;
  UErrorCode status = 0;
  api.canonicalId(id.data(), static_cast<int32_t>(id.size()), canonical, 128, &isSystem, &status);
  if (status > 0 || !isSystem) return false;
  status = 0;
  void* cal = api.open(id.data(), static_cast<int32_t>(id.size()), "en_US", kUcalGregorian, &status);
  if (status > 0 || !cal) {
    if (cal) api.close(cal);
    return false;
  }
  z.backend = Zone::Backend::Icu;
  z.ucal = cal;
  return true;
}

Period icuPeriodAt(Zone& z, int64_t utc) {
  const IcuApi& api = icu();
  Period p;
  std::lock_guard<std::mutex> lock(z.nativeMutex);
  int64_t t = utc;
  if (t > kIcuClampSeconds) t = kIcuClampSeconds;
  if (t < -kIcuClampSeconds) t = -kIcuClampSeconds;
  UErrorCode status = 0;
  api.setMillis(z.ucal, static_cast<UDate>(t) * 1000.0, &status);
  const int32_t zoneMs = api.get(z.ucal, kUcalZoneOffset, &status);
  const int32_t dstMs = api.get(z.ucal, kUcalDstOffset, &status);
  if (status > 0) {
    TRINO_LOG_DEBUG(kLog, "ICU offset lookup failed (status %d)", status);
    return p;
  }
  p.offset = (zoneMs + dstMs) / 1000;
  UDate d = 0;
  status = 0;
  if (api.transitionDate(z.ucal, kUcalTransitionPreviousInclusive, &d, &status) && status <= 0) {
    p.start = static_cast<int64_t>(d / 1000.0);
    if (p.start > utc) p.start = utc;  // ICU rounds to milliseconds
  }
  status = 0;
  if (api.transitionDate(z.ucal, kUcalTransitionNext, &d, &status) && status <= 0) {
    p.end = static_cast<int64_t>(d / 1000.0);
    if (p.end <= utc) p.end = utc + 1;
  }
  return p;
}

using EnumDynamicTimeZoneInformationFn = DWORD(WINAPI*)(DWORD, PDYNAMIC_TIME_ZONE_INFORMATION);
using GetTimeZoneInformationForYearFn = BOOL(WINAPI*)(USHORT, PDYNAMIC_TIME_ZONE_INFORMATION, LPTIME_ZONE_INFORMATION);

struct WinTzApi {
  EnumDynamicTimeZoneInformationFn enumerate = nullptr;
  GetTimeZoneInformationForYearFn forYear = nullptr;
};

const WinTzApi& winTzApi() {
  static const WinTzApi api = [] {
    WinTzApi a;
    // Resolved dynamically: EnumDynamicTimeZoneInformation lives in advapi32 and older SDKs do not
    // declare it, while GetTimeZoneInformationForYear is Vista+ in kernel32.
    if (HMODULE adv = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      loadSymbol(adv, "EnumDynamicTimeZoneInformation", a.enumerate);
    }
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) loadSymbol(k32, "GetTimeZoneInformationForYear", a.forYear);
    return a;
  }();
  return api;
}

bool findDynamicZone(const wchar_t* keyName, DYNAMIC_TIME_ZONE_INFORMATION& out) {
  const WinTzApi& api = winTzApi();
  if (!api.enumerate) return false;
  for (DWORD i = 0;; ++i) {
    DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
    const DWORD rc = api.enumerate(i, &dtzi);
    if (rc != ERROR_SUCCESS) return false;
    if (_wcsicmp(dtzi.TimeZoneKeyName, keyName) == 0) {
      out = dtzi;
      return true;
    }
  }
}

bool openWinRuleZone(std::string_view name, Zone& z) {
  const char* windowsName = ianaToWindows(name);
  if (!windowsName) return false;
  const std::wstring key = utf8ToUtf16Local(windowsName);
  if (!findDynamicZone(key.c_str(), z.dtzi)) return false;
  z.backend = Zone::Backend::WinRule;
  return true;
}

/// Local epoch seconds of a SYSTEMTIME transition rule (wYear == 0: "nth weekday of month").
int64_t systemTimeLocalSeconds(const SYSTEMTIME& st, int64_t year) {
  int64_t days;
  if (st.wYear != 0) {
    days = daysFromCivil(st.wYear, st.wMonth, st.wDay);
  } else {
    const int64_t first = daysFromCivil(year, st.wMonth, 1);
    const int firstWd = weekdayOfDays(first);
    int day = 1 + ((st.wDayOfWeek - firstWd + 7) % 7) + (st.wDay - 1) * 7;
    const int dim = daysInMonth(year, st.wMonth);
    while (day > dim) day -= 7;
    days = first + day - 1;
  }
  return days * kSecondsPerDay + st.wHour * 3600LL + st.wMinute * 60LL + st.wSecond;
}

Period winRulePeriodAt(Zone& z, int64_t utc) {
  const WinTzApi& api = winTzApi();
  const int32_t approx = -static_cast<int32_t>(z.dtzi.Bias) * 60;
  return periodFromYearRules(utc, approx, [&](int64_t y) {
    TIME_ZONE_INFORMATION tzi{};
    bool ok = false;
    if (api.forYear) {
      // GetTimeZoneInformationForYear only accepts years the registry can express.
      const int64_t yy = y < 1601 ? 1601 : (y > 30827 ? 30827 : y);
      std::lock_guard<std::mutex> lock(z.nativeMutex);
      ok = api.forYear(static_cast<USHORT>(yy), &z.dtzi, &tzi) != FALSE;
    }
    if (!ok) {
      tzi.Bias = z.dtzi.Bias;
      tzi.StandardDate = z.dtzi.StandardDate;
      tzi.StandardBias = z.dtzi.StandardBias;
      tzi.DaylightDate = z.dtzi.DaylightDate;
      tzi.DaylightBias = z.dtzi.DaylightBias;
    }
    YearRule r;
    r.stdOffset = -static_cast<int32_t>(tzi.Bias + tzi.StandardBias) * 60;
    r.dstOffset = -static_cast<int32_t>(tzi.Bias + tzi.DaylightBias) * 60;
    r.hasDst = tzi.StandardDate.wMonth != 0 && tzi.DaylightDate.wMonth != 0 && r.stdOffset != r.dstOffset;
    if (r.hasDst) {
      r.dstStartUtc = systemTimeLocalSeconds(tzi.DaylightDate, y) - r.stdOffset;
      r.dstEndUtc = systemTimeLocalSeconds(tzi.StandardDate, y) - r.dstOffset;
    }
    return r;
  });
}
#endif  // TRINO_ODBC_WINDOWS

Zone::~Zone() {
#if defined(TRINO_ODBC_WINDOWS)
  if (ucal && icu().close) icu().close(ucal);
#endif
}

Period periodAt(Zone& z, int64_t utc) {
  switch (z.backend) {
    case Zone::Backend::Fixed: {
      Period p;
      p.offset = z.fixedOffset;
      return p;
    }
    case Zone::Backend::Table:
      return tablePeriodAt(z, utc);
    case Zone::Backend::Rule:
      return rulePeriodAt(z.rule, utc);
    case Zone::Backend::Icu:
#if defined(TRINO_ODBC_WINDOWS)
      return icuPeriodAt(z, utc);
#else
      break;
#endif
    case Zone::Backend::WinRule:
#if defined(TRINO_ODBC_WINDOWS)
      return winRulePeriodAt(z, utc);
#else
      break;
#endif
  }
  return Period{};
}

/// Wall-clock -> offset. Walks the periods around the instant in time order and takes the first one
/// that maps the wall clock consistently, which is the pre-transition offset for overlaps; for gaps
/// (no consistent period) the offset in effect before the gap is used, as java.time does.
int32_t offsetAtLocal(Zone& z, int64_t local) {
  Period p = periodAt(z, local - kOffsetWindow);
  bool havePassed = false;
  int32_t passed = 0;
  for (int iter = 0; iter < 64; ++iter) {
    const int64_t u = local - p.offset;
    if (u >= p.start && u < p.end) return p.offset;
    if (u < p.start) return havePassed ? passed : p.offset;
    passed = p.offset;
    havePassed = true;
    if (p.end == kMaxTime) return p.offset;
    p = periodAt(z, p.end);
  }
  return p.offset;
}

// ---- System zone discovery ----------------------------------------------------------------------

#if defined(TRINO_ODBC_UNIX)
/// Extracts "Area/City" from a path like /usr/share/zoneinfo/Area/City (posix/ and right/ prefixes dropped).
std::string zoneNameFromPath(std::string_view path) {
  const size_t pos = path.rfind("zoneinfo/");
  if (pos == std::string_view::npos) return std::string();
  std::string_view rest = path.substr(pos + 9);
  if (startsWith(rest, "posix/")) rest.remove_prefix(6);
  else if (startsWith(rest, "right/")) rest.remove_prefix(6);
  return std::string(rest);
}

std::string readSymlinkTarget(const char* path) {
  char buf[PATH_MAX + 1];
  const ssize_t n = ::readlink(path, buf, PATH_MAX);
  if (n <= 0) return std::string();
  return std::string(buf, static_cast<size_t>(n));
}
#endif

std::string detectSystemZoneName() {
  TimeZoneDb& db = TimeZoneDb::global();
  auto usable = [&db](std::string_view name) { return !name.empty() && validZoneName(name) && db.isKnown(name); };
#if defined(TRINO_ODBC_WINDOWS)
  DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
  if (GetDynamicTimeZoneInformation(&dtzi) == TIME_ZONE_ID_INVALID) return std::string();
  const std::string key = utf16ToUtf8Local(dtzi.TimeZoneKeyName, -1);
  if (key.empty()) return std::string();
  const IcuApi& api = icu();
  if (api.idForWindowsId) {
    const std::vector<UChar> wkey = toUChars(key);
    UChar out[128];
    UErrorCode status = 0;
    const int32_t n = api.idForWindowsId(wkey.data(), static_cast<int32_t>(wkey.size()), nullptr, out, 128, &status);
    if (status <= 0 && n > 0 && n < 128) {
      const std::wstring w(out, out + n);
      const std::string iana = utf16ToUtf8Local(w.c_str(), n);
      if (usable(iana)) return iana;
    }
  }
  if (const char* iana = windowsToIana(key)) {
    if (usable(iana)) return iana;
  }
  // Degrade instead of guessing: without ICU (or registry rules) for this zone the driver cannot
  // convert with it, so the session zone is left unset and the server's default applies.
  TRINO_LOG_WARN(kLog, "Windows time zone '%s' could not be resolved to a usable IANA zone%s; "
                 "X-Trino-Time-Zone is not sent (set TimeZone= to choose one)",
                 key.c_str(), api.available() ? "" : " (icu.dll unavailable)");
  return std::string();
#else
  std::string tz = envValue("TZ");
  if (!tz.empty()) {
    if (tz.front() == ':') tz.erase(0, 1);
    if (!tz.empty() && tz.front() == '/') {
      const std::string fromPath = zoneNameFromPath(tz);
      if (usable(fromPath)) return fromPath;
    } else if (usable(tz)) {
      return tz;
    }
  }
  const std::string link = readSymlinkTarget("/etc/localtime");
  if (!link.empty()) {
    const std::string fromLink = zoneNameFromPath(link);
    if (usable(fromLink)) return fromLink;
  }
  std::string content;
  if (readSmallFile("/etc/timezone", content)) {
    const std::string name(trim(std::string_view(content).substr(0, content.find('\n'))));
    if (usable(name)) return name;
  }
  return std::string();
#endif
}

}  // namespace

// ---- TimeZoneDb -----------------------------------------------------------------------------

struct TimeZoneDb::Impl {
  std::shared_mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<Zone>> cache;  // nullptr = known-unresolvable

  std::shared_ptr<Zone> resolve(std::string_view name);
  static std::shared_ptr<Zone> build(std::string_view name);
};

std::shared_ptr<Zone> TimeZoneDb::Impl::build(std::string_view name) {
  int32_t fixed = 0;
  if (builtinOffset(name, &fixed)) {
    auto z = std::make_shared<Zone>();
    z->backend = Zone::Backend::Fixed;
    z->fixedOffset = fixed;
    return z;
  }
  auto z = std::make_shared<Zone>();
  if (loadZoneFile(name, *z)) return z;
#if defined(TRINO_ODBC_WINDOWS)
  if (openIcuZone(name, *z)) return z;
  if (openWinRuleZone(name, *z)) return z;
#endif
  // A bare POSIX TZ string ("EST5EDT,M3.2.0,M11.1.0") is accepted as a last resort; every such string
  // contains a digit, which cheaply excludes ordinary unknown IANA names.
  if (name.find_first_of("0123456789") != std::string_view::npos && parsePosixTz(name, z->rule)) {
    z->backend = Zone::Backend::Rule;
    z->hasRule = true;
    return z;
  }
  // Last resort for the driver's default session zone (and its aliases): a fixed offset, so the
  // default works even without zoneinfo files, ICU or registry rules.
  if (fixedZoneFallback(name, &fixed)) {
    TRINO_LOG_WARN(kLog, "time zone '%.*s' not found in the zone database; using its compiled-in fixed offset",
                   static_cast<int>(name.size()), name.data());
    z->backend = Zone::Backend::Fixed;
    z->fixedOffset = fixed;
    return z;
  }
  return nullptr;
}

std::shared_ptr<Zone> TimeZoneDb::Impl::resolve(std::string_view rawName) {
  const std::string_view name = trim(rawName);
  if (name.empty()) return nullptr;
  const std::string key(name);
  {
    std::shared_lock<std::shared_mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
  }
  std::unique_lock<std::shared_mutex> lock(mutex);
  const auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  std::shared_ptr<Zone> zone = build(name);
  if (!zone) TRINO_LOG_DEBUG(kLog, "unknown time zone '%s'", key.c_str());
  cache.emplace(key, zone);
  return zone;
}

TimeZoneDb::TimeZoneDb() : impl_(new Impl()) {}
TimeZoneDb::~TimeZoneDb() = default;

TimeZoneDb& TimeZoneDb::global() {
  static TimeZoneDb instance;
  return instance;
}

bool TimeZoneDb::utcOffsetAtUtc(std::string_view zoneName, int64_t utcEpochSeconds, int32_t* offsetSeconds) {
  std::shared_ptr<Zone> z = impl_->resolve(zoneName);
  if (!z) return false;
  const int32_t off = periodAt(*z, clampTime(utcEpochSeconds)).offset;
  if (offsetSeconds) *offsetSeconds = off;
  return true;
}

bool TimeZoneDb::utcOffsetAtLocal(std::string_view zoneName, int64_t localEpochSeconds, int32_t* offsetSeconds) {
  std::shared_ptr<Zone> z = impl_->resolve(zoneName);
  if (!z) return false;
  const int32_t off = offsetAtLocal(*z, clampTime(localEpochSeconds));
  if (offsetSeconds) *offsetSeconds = off;
  return true;
}

bool TimeZoneDb::isKnown(std::string_view zoneName) { return impl_->resolve(zoneName) != nullptr; }

std::string TimeZoneDb::systemZoneName() {
  static const std::string name = detectSystemZoneName();
  return name;
}

bool fixedZoneFallback(std::string_view zoneName, int32_t* offsetSeconds) {
  // Zones whose current rules are a fixed offset. Turkey: UTC+03:00 all year since 2016-09-07
  // (instants before that date may differ from the real history).
  struct Entry { std::string_view name; int32_t offset; };
  static constexpr Entry kFallbacks[] = {
      {"Europe/Istanbul", 3 * 3600}, {"Asia/Istanbul", 3 * 3600}, {"Turkey", 3 * 3600}};
  const std::string_view name = trim(zoneName);
  for (const Entry& e : kFallbacks) {
    if (iequals(name, e.name)) {
      if (offsetSeconds) *offsetSeconds = e.offset;
      return true;
    }
  }
  return false;
}

bool parseFixedOffset(std::string_view text, int32_t* offsetSeconds) {
  std::string_view t = trim(text);
  if (t.empty()) return false;
  if (iequals(t, "Z") || iequals(t, "UTC") || iequals(t, "GMT") || iequals(t, "UT")) {
    if (offsetSeconds) *offsetSeconds = 0;
    return true;
  }
  // "UTC+05:30" / "GMT-8" (java.time style; the sign is *not* inverted, unlike Etc/GMT+5).
  for (const std::string_view prefix : {"UTC", "GMT", "UT"}) {
    if (t.size() > prefix.size() && istartsWith(t, prefix) && (t[prefix.size()] == '+' || t[prefix.size()] == '-')) {
      t.remove_prefix(prefix.size());
      break;
    }
  }
  if (t.size() < 2 || (t[0] != '+' && t[0] != '-')) return false;
  const bool negative = t[0] == '-';
  const std::string_view body = t.substr(1);
  int hours = 0, minutes = 0, seconds = 0;
  auto allDigits = [](std::string_view s) {
    for (const char c : s) {
      if (c < '0' || c > '9') return false;
    }
    return !s.empty();
  };
  auto toInt = [](std::string_view s) {
    int v = 0;
    for (const char c : s) v = v * 10 + (c - '0');
    return v;
  };
  if (body.find(':') != std::string_view::npos) {
    const std::vector<std::string> parts = split(body, ':', false, false);
    if (parts.size() < 2 || parts.size() > 3) return false;
    if (parts[0].empty() || parts[0].size() > 2 || parts[1].size() != 2 || !allDigits(parts[0]) || !allDigits(parts[1])) return false;
    hours = toInt(parts[0]);
    minutes = toInt(parts[1]);
    if (parts.size() == 3) {
      if (parts[2].size() != 2 || !allDigits(parts[2])) return false;
      seconds = toInt(parts[2]);
    }
  } else {
    if (!allDigits(body)) return false;
    switch (body.size()) {
      case 1:
      case 2:
        hours = toInt(body);
        break;
      case 4:
        hours = toInt(body.substr(0, 2));
        minutes = toInt(body.substr(2, 2));
        break;
      case 6:
        hours = toInt(body.substr(0, 2));
        minutes = toInt(body.substr(2, 2));
        seconds = toInt(body.substr(4, 2));
        break;
      default:
        return false;
    }
  }
  if (hours > 18 || minutes > 59 || seconds > 59) return false;
  const int32_t total = hours * 3600 + minutes * 60 + seconds;
  if (total > 18 * 3600) return false;
  if (offsetSeconds) *offsetSeconds = negative ? -total : total;
  return true;
}

}  // namespace trino_odbc
