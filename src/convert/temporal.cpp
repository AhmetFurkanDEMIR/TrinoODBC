// Civil date/time parsing & formatting for Trino textual values (see temporal.h for conventions).
#include "convert/temporal.h"

#include "util/log.h"
#include "util/strings.h"
#include "util/tz.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

namespace trino_odbc {

namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kNanosPerSecond = 1000000000LL;
// Trino permits zone offsets in [-14:00, +14:00]; we accept a little more so that unusual historical
// offsets stored in text never fail to parse.
constexpr int32_t kMaxOffsetSeconds = 18 * 3600;

/// Reads between minDigits and maxDigits decimal digits at pos. Returns false when fewer than
/// minDigits are present; *count receives the digits consumed.
bool readDigits(std::string_view s, size_t& pos, int minDigits, int maxDigits, int64_t& out, int* count = nullptr) {
  int64_t v = 0;
  int n = 0;
  while (pos < s.size() && n < maxDigits && s[pos] >= '0' && s[pos] <= '9') {
    v = v * 10 + (s[pos] - '0');
    ++pos;
    ++n;
  }
  if (count) *count = n;
  if (n < minDigits) return false;
  out = v;
  return true;
}

bool expectChar(std::string_view s, size_t& pos, char c) {
  if (pos < s.size() && s[pos] == c) {
    ++pos;
    return true;
  }
  return false;
}

/// "Z", "+HH", "+HHMM", "+HH:MM", "+HH:MM:SS" -> seconds east of UTC.
bool parseOffsetToken(std::string_view s, int32_t* out) {
  if (s.empty()) return false;
  if (s.size() == 1 && (s[0] == 'Z' || s[0] == 'z')) {
    *out = 0;
    return true;
  }
  if (s[0] != '+' && s[0] != '-') return false;
  const bool negative = s[0] == '-';
  size_t pos = 1;
  int64_t hh = 0, mm = 0, ss = 0;
  if (!readDigits(s, pos, 2, 2, hh)) return false;
  if (pos < s.size()) {
    if (s[pos] == ':') {
      ++pos;
      if (!readDigits(s, pos, 2, 2, mm)) return false;
      if (pos < s.size()) {
        if (s[pos] != ':') return false;
        ++pos;
        if (!readDigits(s, pos, 2, 2, ss)) return false;
      }
    } else {
      if (!readDigits(s, pos, 2, 2, mm)) return false;
      if (pos < s.size()) {
        if (!readDigits(s, pos, 2, 2, ss)) return false;
      }
    }
  }
  if (pos != s.size() || mm > 59 || ss > 59) return false;
  const int64_t total = hh * 3600 + mm * 60 + ss;
  if (total > kMaxOffsetSeconds) return false;
  *out = static_cast<int32_t>(negative ? -total : total);
  return true;
}

/// Parses "HH:MM:SS[.fraction]" starting at pos; leaves pos after the last consumed character.
bool parseClock(std::string_view s, size_t& pos, CivilTime& out) {
  int64_t h = 0, m = 0, sec = 0;
  if (!readDigits(s, pos, 1, 2, h) || !expectChar(s, pos, ':')) return false;
  if (!readDigits(s, pos, 1, 2, m) || !expectChar(s, pos, ':')) return false;
  if (!readDigits(s, pos, 1, 2, sec)) return false;
  if (h > 23 || m > 59 || sec > 59) return false;
  int64_t nanos = 0;
  int digits = 0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    // Trino prints up to 12 fractional digits (picoseconds); we keep nanoseconds and remember the
    // digit count so that formatting can reproduce the column precision.
    const size_t start = pos;
    while (pos < s.size() && digits < 12 && s[pos] >= '0' && s[pos] <= '9') {
      if (digits < 9) nanos = nanos * 10 + (s[pos] - '0');
      ++pos;
      ++digits;
    }
    if (digits == 0 || pos == start) return false;
    for (int i = digits; i < 9; ++i) nanos *= 10;
  }
  out.hour = static_cast<int>(h);
  out.minute = static_cast<int>(m);
  out.second = static_cast<int>(sec);
  out.nanos = nanos;
  out.precision = digits;
  return true;
}

/// Parses "[+-]Y+-M-D" starting at pos.
bool parseDatePart(std::string_view s, size_t& pos, CivilDate& out) {
  bool negative = false;
  if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) {
    negative = s[pos] == '-';
    ++pos;
  }
  int64_t y = 0, m = 0, d = 0;
  if (!readDigits(s, pos, 1, 10, y) || !expectChar(s, pos, '-')) return false;
  if (!readDigits(s, pos, 1, 2, m) || !expectChar(s, pos, '-')) return false;
  if (!readDigits(s, pos, 1, 2, d)) return false;
  if (negative) y = -y;
  if (y < std::numeric_limits<int32_t>::min() || y > std::numeric_limits<int32_t>::max()) return false;
  if (!isValidDate(static_cast<int32_t>(y), static_cast<int>(m), static_cast<int>(d))) return false;
  out.year = static_cast<int32_t>(y);
  out.month = static_cast<int>(m);
  out.day = static_cast<int>(d);
  return true;
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

bool isZoneNameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' ||
         c == '-' || c == '+';
}

void appendTwoDigits(std::string& out, int v) {
  out.push_back(static_cast<char>('0' + (v / 10) % 10));
  out.push_back(static_cast<char>('0' + v % 10));
}

void appendUnsigned(std::string& out, uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out += buf;
}

}  // namespace

bool isValidDate(int32_t y, int m, int d) {
  if (m < 1 || m > 12 || d < 1) return false;
  static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = dim[m - 1];
  if (m == 2) {
    const bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    if (leap) days = 29;
  }
  return d <= days;
}

int64_t daysFromCivil(int32_t y, int m, int d) {
  // Howard Hinnant's proleptic Gregorian algorithm (valid for the whole int32 year range).
  int64_t yy = y;
  yy -= m <= 2;
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const int64_t yoe = yy - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t days, int32_t& y, int& m, int& d) {
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) ++year;
  y = static_cast<int32_t>(year);
}

bool parseTrinoDate(std::string_view s, CivilDate& out) {
  s = trim(s);
  size_t pos = 0;
  CivilDate tmp;
  if (!parseDatePart(s, pos, tmp) || pos != s.size()) return false;
  out = tmp;
  return true;
}

bool parseTrinoTime(std::string_view s, CivilTime& out, int32_t* offsetSeconds, bool* hasOffset) {
  s = trim(s);
  size_t pos = 0;
  CivilTime tmp;
  if (!parseClock(s, pos, tmp)) return false;
  bool gotOffset = false;
  int32_t offset = 0;
  if (pos < s.size()) {
    std::string_view rest = s.substr(pos);
    if (!rest.empty() && rest[0] == ' ') rest = trim(rest);
    if (!parseOffsetToken(rest, &offset)) return false;
    gotOffset = true;
  }
  out = tmp;
  if (offsetSeconds) *offsetSeconds = gotOffset ? offset : 0;
  if (hasOffset) *hasOffset = gotOffset;
  return true;
}

bool parseTrinoTimestamp(std::string_view s, ParsedTimestamp& out) {
  s = trim(s);
  size_t pos = 0;
  ParsedTimestamp tmp;
  if (!parseDatePart(s, pos, tmp.date)) return false;
  // Trino separates with a space; ISO text uses 'T' — accept both for char -> timestamp conversions.
  if (pos >= s.size() || (s[pos] != ' ' && s[pos] != 'T' && s[pos] != 't')) return false;
  ++pos;
  if (!parseClock(s, pos, tmp.time)) return false;
  if (pos < s.size()) {
    std::string_view rest = s.substr(pos);
    const bool hadSpace = rest[0] == ' ';
    rest = trim(rest);
    if (rest.empty()) return false;
    int32_t offset = 0;
    if (parseOffsetToken(rest, &offset)) {
      tmp.hasZone = true;
      tmp.zoneIsOffset = true;
      tmp.offsetSeconds = offset;
    } else {
      // A zone name must be separated by a space and consist of IANA-name characters only.
      if (!hadSpace) return false;
      for (char c : rest) {
        if (!isZoneNameChar(c)) return false;
      }
      tmp.hasZone = true;
      tmp.zoneIsOffset = false;
      tmp.zoneName.assign(rest.data(), rest.size());
    }
  }
  out = std::move(tmp);
  return true;
}

CivilDate currentCivilDate(std::string_view sessionTimeZone) {
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  int32_t offset = 0;
  if (!sessionTimeZone.empty() && TimeZoneDb::global().utcOffsetAtUtc(sessionTimeZone, now, &offset)) {
    CivilDate d;
    civilFromDays(floorDiv(now + offset, kSecondsPerDay), d.year, d.month, d.day);
    return d;
  }
  const std::time_t t = static_cast<std::time_t>(now);
  std::tm local;
#if defined(TRINO_ODBC_WINDOWS)
  if (localtime_s(&local, &t) != 0) std::memset(&local, 0, sizeof(local));
#else
  if (!localtime_r(&t, &local)) std::memset(&local, 0, sizeof(local));
#endif
  CivilDate d;
  d.year = local.tm_year + 1900;
  d.month = local.tm_mon + 1;
  d.day = local.tm_mday;
  return d;
}

bool timestampToUtc(ParsedTimestamp& ts) {
  if (!ts.hasZone) return true;
  int32_t offset = 0;
  const int64_t localSeconds = daysFromCivil(ts.date.year, ts.date.month, ts.date.day) * kSecondsPerDay +
                               static_cast<int64_t>(ts.time.hour) * 3600 + ts.time.minute * 60 + ts.time.second;
  if (ts.zoneIsOffset) {
    offset = ts.offsetSeconds;
  } else if (!TimeZoneDb::global().utcOffsetAtLocal(ts.zoneName, localSeconds, &offset)) {
    TRINO_LOG_DEBUG("temporal", "unknown time zone '%s'", ts.zoneName.c_str());
    return false;
  }
  const int64_t utcSeconds = localSeconds - offset;
  const int64_t days = floorDiv(utcSeconds, kSecondsPerDay);
  const int64_t secondOfDay = utcSeconds - days * kSecondsPerDay;
  int32_t y = 0;
  int m = 0, d = 0;
  // Years beyond int32 cannot be represented; leave the value untouched rather than wrap.
  if (days > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * 366 ||
      days < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) * 366) {
    return false;
  }
  civilFromDays(days, y, m, d);
  ts.date.year = y;
  ts.date.month = m;
  ts.date.day = d;
  ts.time.hour = static_cast<int>(secondOfDay / 3600);
  ts.time.minute = static_cast<int>((secondOfDay % 3600) / 60);
  ts.time.second = static_cast<int>(secondOfDay % 60);
  ts.hasZone = true;
  ts.zoneIsOffset = true;
  ts.offsetSeconds = 0;
  ts.zoneName.clear();
  return true;
}

namespace {

/// Rewrites date/time from epoch seconds of a wall clock ("as if UTC"). False when out of range.
bool setFromLocalSeconds(ParsedTimestamp& ts, int64_t localSeconds) {
  const int64_t days = floorDiv(localSeconds, kSecondsPerDay);
  if (days > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) * 366 ||
      days < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) * 366) {
    return false;
  }
  const int64_t secondOfDay = localSeconds - days * kSecondsPerDay;
  int32_t y = 0;
  int m = 0, d = 0;
  civilFromDays(days, y, m, d);
  ts.date.year = y;
  ts.date.month = m;
  ts.date.day = d;
  ts.time.hour = static_cast<int>(secondOfDay / 3600);
  ts.time.minute = static_cast<int>((secondOfDay % 3600) / 60);
  ts.time.second = static_cast<int>(secondOfDay % 60);
  return true;
}

int64_t localSecondsOf(const ParsedTimestamp& ts) {
  return daysFromCivil(ts.date.year, ts.date.month, ts.date.day) * kSecondsPerDay +
         static_cast<int64_t>(ts.time.hour) * 3600 + ts.time.minute * 60 + ts.time.second;
}

void shiftTimeOfDay(CivilTime& t, int64_t deltaSeconds) {
  int64_t secs = static_cast<int64_t>(t.hour) * 3600 + t.minute * 60 + t.second + deltaSeconds;
  secs = secs - floorDiv(secs, kSecondsPerDay) * kSecondsPerDay;
  t.hour = static_cast<int>(secs / 3600);
  t.minute = static_cast<int>((secs % 3600) / 60);
  t.second = static_cast<int>(secs % 60);
}

}  // namespace

ZoneConversion timestampToZone(ParsedTimestamp& ts, std::string_view zone) {
  if (!ts.hasZone) return ZoneConversion::Ok;
  if (!timestampToUtc(ts)) return ZoneConversion::UnknownValueZone;
  const int64_t utcSeconds = localSecondsOf(ts);
  int32_t offset = 0;
  if (trim(zone).empty() || !TimeZoneDb::global().utcOffsetAtUtc(zone, utcSeconds, &offset)) {
    TRINO_LOG_DEBUG("temporal", "unknown target time zone '%.*s'", static_cast<int>(zone.size()), zone.data());
    return ZoneConversion::UnknownTargetZone;
  }
  if (offset != 0) {
    ParsedTimestamp shifted = ts;
    if (!setFromLocalSeconds(shifted, utcSeconds + offset)) return ZoneConversion::UnknownTargetZone;
    ts = std::move(shifted);
  }
  ts.hasZone = true;
  ts.zoneIsOffset = true;
  ts.offsetSeconds = offset;
  return ZoneConversion::Ok;
}

bool timeToZone(CivilTime& t, int32_t offsetSeconds, std::string_view zone) {
  int32_t target = 0;
  const bool known = !trim(zone).empty() &&
                     TimeZoneDb::global().utcOffsetAtUtc(zone, static_cast<int64_t>(std::time(nullptr)), &target);
  shiftTimeOfDay(t, static_cast<int64_t>(known ? target : 0) - offsetSeconds);
  return known;
}

std::string formatDate(const CivilDate& d) {
  std::string out;
  out.reserve(12);
  int64_t y = d.year;
  if (y < 0) {
    out.push_back('-');
    y = -y;
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04lld", static_cast<long long>(y));
  out += buf;
  out.push_back('-');
  appendTwoDigits(out, d.month);
  out.push_back('-');
  appendTwoDigits(out, d.day);
  return out;
}

std::string formatTime(const CivilTime& t, int fractionDigits) {
  std::string out;
  out.reserve(24);
  appendTwoDigits(out, t.hour);
  out.push_back(':');
  appendTwoDigits(out, t.minute);
  out.push_back(':');
  appendTwoDigits(out, t.second);
  if (fractionDigits > 0) {
    if (fractionDigits > 12) fractionDigits = 12;
    char buf[16];
    int64_t nanos = t.nanos;
    if (nanos < 0) nanos = 0;
    if (nanos >= kNanosPerSecond) nanos = kNanosPerSecond - 1;
    std::snprintf(buf, sizeof(buf), "%09lld", static_cast<long long>(nanos));
    out.push_back('.');
    for (int i = 0; i < fractionDigits; ++i) out.push_back(i < 9 ? buf[i] : '0');
  }
  return out;
}

std::string formatTimestamp(const CivilDate& d, const CivilTime& t, int fractionDigits) {
  std::string out = formatDate(d);
  out.push_back(' ');
  out += formatTime(t, fractionDigits);
  return out;
}

bool parseTrinoIntervalYearMonth(std::string_view s, SQL_INTERVAL_STRUCT& out) {
  s = trim(s);
  size_t pos = 0;
  bool negative = false;
  if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) {
    negative = s[pos] == '-';
    ++pos;
  }
  int64_t years = 0, months = 0;
  if (!readDigits(s, pos, 1, 10, years) || !expectChar(s, pos, '-')) return false;
  if (!readDigits(s, pos, 1, 10, months) || pos != s.size()) return false;
  const int64_t maxField = std::numeric_limits<SQLUINTEGER>::max();
  if (years > maxField || months > maxField) return false;
  std::memset(&out, 0, sizeof(out));
  out.interval_type = SQL_IS_YEAR_TO_MONTH;
  out.interval_sign = negative ? SQL_TRUE : SQL_FALSE;
  out.intval.year_month.year = static_cast<SQLUINTEGER>(years);
  out.intval.year_month.month = static_cast<SQLUINTEGER>(months);
  return true;
}

bool parseTrinoIntervalDaySecond(std::string_view s, SQL_INTERVAL_STRUCT& out) {
  s = trim(s);
  size_t pos = 0;
  bool negative = false;
  if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) {
    negative = s[pos] == '-';
    ++pos;
  }
  int64_t days = 0, hours = 0, minutes = 0, seconds = 0, nanos = 0;
  // Trino always prints the day field; a bare "HH:MM:SS" (day omitted) is accepted for user text.
  const size_t space = s.find(' ', pos);
  if (space != std::string_view::npos) {
    if (!readDigits(s, pos, 1, 10, days) || pos != space) return false;
    ++pos;
    while (pos < s.size() && s[pos] == ' ') ++pos;
  }
  if (!readDigits(s, pos, 1, 10, hours) || !expectChar(s, pos, ':')) return false;
  if (!readDigits(s, pos, 1, 2, minutes) || !expectChar(s, pos, ':')) return false;
  if (!readDigits(s, pos, 1, 2, seconds)) return false;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (digits < 9) nanos = nanos * 10 + (s[pos] - '0');
      ++pos;
      ++digits;
    }
    if (digits == 0) return false;
    for (int i = digits; i < 9; ++i) nanos *= 10;
  }
  if (pos != s.size()) return false;
  const int64_t maxField = std::numeric_limits<SQLUINTEGER>::max();
  if (days > maxField || hours > maxField || minutes > 59 || seconds > 59) return false;
  std::memset(&out, 0, sizeof(out));
  out.interval_type = SQL_IS_DAY_TO_SECOND;
  out.interval_sign = negative ? SQL_TRUE : SQL_FALSE;
  out.intval.day_second.day = static_cast<SQLUINTEGER>(days);
  out.intval.day_second.hour = static_cast<SQLUINTEGER>(hours);
  out.intval.day_second.minute = static_cast<SQLUINTEGER>(minutes);
  out.intval.day_second.second = static_cast<SQLUINTEGER>(seconds);
  out.intval.day_second.fraction = static_cast<SQLUINTEGER>(nanos);
  return true;
}

std::string formatIntervalYearMonth(const SQL_INTERVAL_STRUCT& v) {
  std::string out;
  if (v.interval_sign == SQL_TRUE) out.push_back('-');
  appendUnsigned(out, v.intval.year_month.year);
  out.push_back('-');
  appendUnsigned(out, v.intval.year_month.month);
  return out;
}

std::string formatIntervalDaySecond(const SQL_INTERVAL_STRUCT& v) {
  std::string out;
  if (v.interval_sign == SQL_TRUE) out.push_back('-');
  const SQL_DAY_SECOND_STRUCT& ds = v.intval.day_second;
  appendUnsigned(out, ds.day);
  out.push_back(' ');
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", static_cast<unsigned long long>(ds.hour),
                static_cast<unsigned long long>(ds.minute), static_cast<unsigned long long>(ds.second));
  out += buf;
  // Trino's canonical text carries milliseconds; keep more digits only when they are non-zero.
  uint64_t nanos = ds.fraction;
  if (nanos >= static_cast<uint64_t>(kNanosPerSecond)) nanos = kNanosPerSecond - 1;
  int digits = 3;
  if (nanos % 1000 != 0) {
    digits = 9;
  } else if (nanos % 1000000 != 0) {
    digits = 6;
  }
  std::snprintf(buf, sizeof(buf), "%09llu", static_cast<unsigned long long>(nanos));
  out.push_back('.');
  out.append(buf, static_cast<size_t>(digits));
  return out;
}

}  // namespace trino_odbc
