// Bound parameter -> Trino literal rendering for `EXECUTE [IMMEDIATE] ... USING <lit>, ...`.
//
// Typing precedence: the server-described parameter type (DESCRIBE INPUT) wins over the bound SQL
// type, which wins over the application C type. The C type only says how to read the buffer. Every
// value is validated client side so that a bad buffer surfaces as an ODBC SQLSTATE instead of a
// confusing Trino analysis error, and every literal is typed (DOUBLE '1.5', DECIMAL '1.20',
// TIMESTAMP '...') so Trino never has to guess a type from the literal's shape.
//
// Date/time formatting is self-contained (the subset needed here is tiny); only the current date
// used by time -> timestamp conversions comes from convert/temporal.h, so both conversion
// directions agree on "today" in the session time zone.
#include "convert/params.h"

#include "convert/numeric.h"
#include "convert/temporal.h"
#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/unicode.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "params";
constexpr int kTrinoMaxDecimalPrecision = 38;
// Interval seconds precision of an SQL_C_INTERVAL_* parameter buffer: the ODBC default of the APD
// SQL_DESC_PRECISION (6, i.e. day_second.fraction counts microseconds). It matches what SQLGetData
// writes by default, so a value read from the driver binds back unchanged.
constexpr int kIntervalSecondsPrecision = 6;

// ---- errors ------------------------------------------------------------------------------------

[[noreturn]] void throwError(const char* sqlstate, std::string message) {
  throw DriverError(sqlstate, std::move(message));
}

[[noreturn]] void throwRestricted(SQLSMALLINT cType, std::string_view targetType) {
  throwError(sqlstate::kRestrictedDataType,
             formatString("Cannot convert parameter of C type %s to Trino type %.*s", cTypeName(cType),
                          static_cast<int>(targetType.size()), targetType.data()));
}

[[noreturn]] void throwInvalidValue(std::string_view text, std::string_view targetType) {
  // Keep the offending text short in diagnostics: it is user data and may be huge.
  const std::string_view shown = text.substr(0, 64);
  throwError(sqlstate::kInvalidCharacterValue,
             formatString("Invalid character value '%.*s%s' for parameter of type %.*s", static_cast<int>(shown.size()),
                          shown.data(), text.size() > shown.size() ? "..." : "", static_cast<int>(targetType.size()),
                          targetType.data()));
}

[[noreturn]] void throwOutOfRange(std::string_view value, std::string_view targetType) {
  const std::string_view shown = value.substr(0, 64);
  throwError(sqlstate::kNumericOutOfRange,
             formatString("Numeric value %.*s out of range for parameter of type %.*s", static_cast<int>(shown.size()),
                          shown.data(), static_cast<int>(targetType.size()), targetType.data()));
}

// ---- target type -------------------------------------------------------------------------------

/// What the parameter must become on the Trino side.
struct Target {
  TypeCategory category = TypeCategory::Varchar;
  std::string typeName = "varchar";  // CAST(NULL AS ...) and generic CAST typing
  SQLSMALLINT sqlType = SQL_UNKNOWN_TYPE;  // ODBC type the target was derived from (interval fields)
  int precision = -1;                // decimal precision, -1 = unconstrained
  int scale = -1;                    // decimal scale, -1 = keep the source scale
  int fractionDigits = -1;           // minimum fractional-second digits, -1 = default (3)
};

TypeCategory categoryForSqlType(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_CHAR: case SQL_WCHAR: return TypeCategory::Char;
    case SQL_VARCHAR: case SQL_LONGVARCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR: return TypeCategory::Varchar;
    case SQL_BIT: return TypeCategory::Boolean;
    case SQL_TINYINT: return TypeCategory::TinyInt;
    case SQL_SMALLINT: return TypeCategory::SmallInt;
    case SQL_INTEGER: return TypeCategory::Integer;
    case SQL_BIGINT: return TypeCategory::BigInt;
    case SQL_REAL: return TypeCategory::Real;
    case SQL_FLOAT: case SQL_DOUBLE: return TypeCategory::Double;
    case SQL_DECIMAL: case SQL_NUMERIC: return TypeCategory::Decimal;
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY: return TypeCategory::Varbinary;
    case SQL_TYPE_DATE: case SQL_DATE: return TypeCategory::Date;
    case SQL_TYPE_TIME: case SQL_TIME: return TypeCategory::Time;
    case SQL_TYPE_TIMESTAMP: case SQL_TIMESTAMP: return TypeCategory::Timestamp;
    case SQL_GUID: return TypeCategory::Uuid;
    case SQL_INTERVAL_YEAR: case SQL_INTERVAL_MONTH: case SQL_INTERVAL_YEAR_TO_MONTH:
      return TypeCategory::IntervalYearToMonth;
    case SQL_INTERVAL_DAY: case SQL_INTERVAL_HOUR: case SQL_INTERVAL_MINUTE: case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_DAY_TO_HOUR: case SQL_INTERVAL_DAY_TO_MINUTE: case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE: case SQL_INTERVAL_HOUR_TO_SECOND: case SQL_INTERVAL_MINUTE_TO_SECOND:
      return TypeCategory::IntervalDayToSecond;
    default: return TypeCategory::Unknown;
  }
}

/// The SQL type a bare C type implies when neither the server nor the application typed the parameter.
/// Unsigned C types widen to the next signed SQL type so the full value range stays representable.
SQLSMALLINT sqlTypeForCType(SQLSMALLINT cType) {
  switch (cType) {
    case SQL_C_CHAR: return SQL_VARCHAR;
    case SQL_C_WCHAR: return SQL_WVARCHAR;
    case SQL_C_BIT: return SQL_BIT;
    case SQL_C_STINYINT: case SQL_C_TINYINT: return SQL_TINYINT;
    case SQL_C_UTINYINT: case SQL_C_SSHORT: case SQL_C_SHORT: return SQL_SMALLINT;
    case SQL_C_USHORT: case SQL_C_SLONG: case SQL_C_LONG: return SQL_INTEGER;
    case SQL_C_ULONG: case SQL_C_SBIGINT: return SQL_BIGINT;
    case SQL_C_UBIGINT: case SQL_C_NUMERIC: return SQL_DECIMAL;
    case SQL_C_FLOAT: return SQL_REAL;
    case SQL_C_DOUBLE: return SQL_DOUBLE;
    case SQL_C_BINARY: return SQL_VARBINARY;
    case SQL_C_TYPE_DATE: return SQL_TYPE_DATE;
    case SQL_C_TYPE_TIME: return SQL_TYPE_TIME;
    case SQL_C_TYPE_TIMESTAMP: return SQL_TYPE_TIMESTAMP;
    case SQL_C_GUID: return SQL_GUID;
    case SQL_C_INTERVAL_YEAR: return SQL_INTERVAL_YEAR;
    case SQL_C_INTERVAL_MONTH: return SQL_INTERVAL_MONTH;
    case SQL_C_INTERVAL_YEAR_TO_MONTH: return SQL_INTERVAL_YEAR_TO_MONTH;
    case SQL_C_INTERVAL_DAY: return SQL_INTERVAL_DAY;
    case SQL_C_INTERVAL_HOUR: return SQL_INTERVAL_HOUR;
    case SQL_C_INTERVAL_MINUTE: return SQL_INTERVAL_MINUTE;
    case SQL_C_INTERVAL_SECOND: return SQL_INTERVAL_SECOND;
    case SQL_C_INTERVAL_DAY_TO_HOUR: return SQL_INTERVAL_DAY_TO_HOUR;
    case SQL_C_INTERVAL_DAY_TO_MINUTE: return SQL_INTERVAL_DAY_TO_MINUTE;
    case SQL_C_INTERVAL_DAY_TO_SECOND: return SQL_INTERVAL_DAY_TO_SECOND;
    case SQL_C_INTERVAL_HOUR_TO_MINUTE: return SQL_INTERVAL_HOUR_TO_MINUTE;
    case SQL_C_INTERVAL_HOUR_TO_SECOND: return SQL_INTERVAL_HOUR_TO_SECOND;
    case SQL_C_INTERVAL_MINUTE_TO_SECOND: return SQL_INTERVAL_MINUTE_TO_SECOND;
    default: return SQL_UNKNOWN_TYPE;
  }
}

Target targetFromSqlType(SQLSMALLINT sqlType, SQLULEN columnSize, SQLSMALLINT decimalDigits) {
  Target t;
  t.category = categoryForSqlType(sqlType);
  t.sqlType = sqlType;
  t.typeName = trinoTypeForSqlType(sqlType, columnSize, decimalDigits);
  if (t.category == TypeCategory::Decimal) {
    // ColumnSize 0 means the application did not constrain the parameter: keep the source digits.
    if (columnSize > 0 && columnSize <= static_cast<SQLULEN>(kTrinoMaxDecimalPrecision)) {
      t.precision = static_cast<int>(columnSize);
      t.scale = decimalDigits < 0 ? 0 : decimalDigits;
      if (t.scale > t.precision) t.scale = t.precision;
    }
  } else if (t.category == TypeCategory::Time || t.category == TypeCategory::Timestamp) {
    t.fractionDigits = decimalDigits < 0 ? 0 : (decimalDigits > 9 ? 9 : decimalDigits);
  }
  return t;
}

/// Resolves the target type by precedence: server type > bound SQL type > C type.
Target resolveTarget(const ParamValue& v, SQLSMALLINT resolvedCType, const std::optional<TrinoType>& serverType) {
  // DESCRIBE INPUT answers "unknown" when Trino could not infer a type; that carries no information.
  const bool serverKnown = serverType.has_value() && !(serverType->category == TypeCategory::Unknown &&
                                                       (serverType->rawType.empty() || iequals(serverType->rawType, "unknown")));
  if (serverKnown) {
    Target t;
    t.category = serverType->category;
    t.typeName = serverType->toString();
    t.sqlType = v.sqlType;
    if (t.category == TypeCategory::Decimal) {
      t.precision = serverType->decimalPrecision();
      t.scale = serverType->decimalScale();
    } else if (serverType->category == TypeCategory::Time || serverType->category == TypeCategory::TimeWithTimeZone ||
               serverType->category == TypeCategory::Timestamp ||
               serverType->category == TypeCategory::TimestampWithTimeZone) {
      const int p = serverType->temporalPrecision();
      t.fractionDigits = p < 0 ? 0 : (p > 9 ? 9 : p);
    }
    return t;
  }
  if (v.sqlType != SQL_UNKNOWN_TYPE && categoryForSqlType(v.sqlType) != TypeCategory::Unknown) {
    return targetFromSqlType(v.sqlType, v.columnSize, v.decimalDigits);
  }
  const SQLSMALLINT implied = sqlTypeForCType(resolvedCType);
  if (implied == SQL_UNKNOWN_TYPE) {
    throwError(sqlstate::kInvalidAppBufferType,
               formatString("Unsupported parameter C type %d (%s)", static_cast<int>(resolvedCType), cTypeName(resolvedCType)));
  }
  // Only the shape of the C type is known; SQL_C_NUMERIC still carries its own precision/scale and
  // temporal values fall back to the default fraction width (3..9 digits, trimmed).
  Target t = targetFromSqlType(implied, resolvedCType == SQL_C_UBIGINT ? 20 : 0, 0);
  t.fractionDigits = -1;
  return t;
}

// ---- reading the application buffer -------------------------------------------------------------

/// The parameter value read from the application buffer, before any conversion.
struct Source {
  enum class Kind { Text, Binary, Int, UInt, Double, Decimal, Bool, Date, Time, Timestamp, Interval, Guid };
  Kind kind = Kind::Text;
  SQLSMALLINT cType = SQL_C_CHAR;
  std::string text;      // Text
  std::string bytes;     // Binary
  int64_t i = 0;         // Int
  uint64_t u = 0;        // UInt
  double d = 0.0;        // Double
  bool isFloat = false;  // Double read from SQL_C_FLOAT (shortest text differs)
  Decimal dec;           // Decimal
  bool b = false;        // Bool
  SQL_DATE_STRUCT date{};
  SQL_TIME_STRUCT time{};
  SQL_TIMESTAMP_STRUCT ts{};
  SQL_INTERVAL_STRUCT iv{};
  bool intervalYearMonth = false;
  SQLGUID guid{};
};

const void* requireData(const ParamValue& v) {
  if (!v.data) throwError(sqlstate::kInvalidNullPointer, "Parameter value pointer is null");
  return v.data;
}

template <typename T>
T readFixed(const ParamValue& v) {
  T out{};
  if (v.daeData) {
    // Data-at-execution for a fixed-size type: the whole struct must arrive in one SQLPutData.
    if (v.daeData->size() < sizeof(T)) {
      throwError(sqlstate::kInvalidStringOrBufferLength,
                 formatString("Data-at-execution buffer holds %zu bytes, %zu required", v.daeData->size(), sizeof(T)));
    }
    std::memcpy(&out, v.daeData->data(), sizeof(T));
    return out;
  }
  std::memcpy(&out, requireData(v), sizeof(T));
  return out;
}

std::string readText(const ParamValue& v, SQLSMALLINT cType, const ConvertOptions& opts) {
  if (cType == SQL_C_WCHAR) {
    if (v.daeData) {
      const size_t bytes = v.daeData->size();
      if (bytes % sizeof(SQLWCHAR) != 0) {
        throwError(sqlstate::kInvalidStringOrBufferLength, "SQL_C_WCHAR data-at-execution length is not a whole number of characters");
      }
      // Copy: std::string storage is not guaranteed to be SQLWCHAR-aligned.
      WString w(bytes / sizeof(SQLWCHAR), SQLWCHAR{});
      if (bytes) std::memcpy(&w[0], v.daeData->data(), bytes);
      return wideToUtf8(w);
    }
    const auto* w = static_cast<const SQLWCHAR*>(requireData(v));
    if (v.length == SQL_NTS) return wideToUtf8(w, static_cast<SQLLEN>(SQL_NTS));
    if (v.length < 0) throwError(sqlstate::kInvalidStringOrBufferLength, formatString("Invalid parameter length %ld", static_cast<long>(v.length)));
    if (v.length % static_cast<SQLLEN>(sizeof(SQLWCHAR)) != 0) {
      throwError(sqlstate::kInvalidStringOrBufferLength, "SQL_C_WCHAR parameter length is not a whole number of characters");
    }
    return wideToUtf8(w, v.length / static_cast<SQLLEN>(sizeof(SQLWCHAR)));
  }
  if (v.daeData) {
    return narrowToUtf8(reinterpret_cast<const SQLCHAR*>(v.daeData->data()), static_cast<SQLLEN>(v.daeData->size()), opts.ansi);
  }
  const auto* p = static_cast<const SQLCHAR*>(requireData(v));
  if (v.length == SQL_NTS) return narrowToUtf8(p, static_cast<SQLLEN>(SQL_NTS), opts.ansi);
  if (v.length < 0) throwError(sqlstate::kInvalidStringOrBufferLength, formatString("Invalid parameter length %ld", static_cast<long>(v.length)));
  return narrowToUtf8(p, v.length, opts.ansi);
}

std::string readBinary(const ParamValue& v) {
  if (v.daeData) return *v.daeData;
  const auto* p = static_cast<const char*>(requireData(v));
  SQLLEN n = v.length;
  if (n == SQL_NTS) {
    // SQL_NTS is meaningless for binary data; the bind's BufferLength is the only usable size.
    if (v.bufferLength <= 0) throwError(sqlstate::kInvalidStringOrBufferLength, "Binary parameter needs an explicit byte length");
    n = v.bufferLength;
  }
  if (n < 0) throwError(sqlstate::kInvalidStringOrBufferLength, formatString("Invalid parameter length %ld", static_cast<long>(n)));
  return std::string(p, static_cast<size_t>(n));
}

Source readSource(const ParamValue& v, SQLSMALLINT cType, const ConvertOptions& opts) {
  Source s;
  s.cType = cType;
  switch (cType) {
    case SQL_C_CHAR: case SQL_C_WCHAR:
      s.kind = Source::Kind::Text;
      s.text = readText(v, cType, opts);
      return s;
    case SQL_C_BINARY:
      s.kind = Source::Kind::Binary;
      s.bytes = readBinary(v);
      return s;
    case SQL_C_BIT:
      s.kind = Source::Kind::Bool;
      s.b = readFixed<SQLCHAR>(v) != 0;
      return s;
    case SQL_C_STINYINT: case SQL_C_TINYINT: s.kind = Source::Kind::Int; s.i = readFixed<SQLSCHAR>(v); return s;
    case SQL_C_UTINYINT: s.kind = Source::Kind::UInt; s.u = readFixed<SQLCHAR>(v); return s;
    case SQL_C_SSHORT: case SQL_C_SHORT: s.kind = Source::Kind::Int; s.i = readFixed<SQLSMALLINT>(v); return s;
    case SQL_C_USHORT: s.kind = Source::Kind::UInt; s.u = readFixed<SQLUSMALLINT>(v); return s;
    case SQL_C_SLONG: case SQL_C_LONG: s.kind = Source::Kind::Int; s.i = readFixed<SQLINTEGER>(v); return s;
    case SQL_C_ULONG: s.kind = Source::Kind::UInt; s.u = readFixed<SQLUINTEGER>(v); return s;
    case SQL_C_SBIGINT: s.kind = Source::Kind::Int; s.i = readFixed<SQLBIGINT>(v); return s;
    case SQL_C_UBIGINT: s.kind = Source::Kind::UInt; s.u = readFixed<SQLUBIGINT>(v); return s;
    case SQL_C_FLOAT:
      s.kind = Source::Kind::Double;
      s.d = static_cast<double>(readFixed<SQLREAL>(v));
      s.isFloat = true;
      return s;
    case SQL_C_DOUBLE:
      s.kind = Source::Kind::Double;
      s.d = readFixed<SQLDOUBLE>(v);
      return s;
    case SQL_C_NUMERIC: {
      s.kind = Source::Kind::Decimal;
      const SQL_NUMERIC_STRUCT num = readFixed<SQL_NUMERIC_STRUCT>(v);
      if (!numericToDecimal(num, s.dec)) throwError(sqlstate::kInvalidCharacterValue, "Malformed SQL_NUMERIC_STRUCT parameter");
      return s;
    }
    case SQL_C_TYPE_DATE: s.kind = Source::Kind::Date; s.date = readFixed<SQL_DATE_STRUCT>(v); return s;
    case SQL_C_TYPE_TIME: s.kind = Source::Kind::Time; s.time = readFixed<SQL_TIME_STRUCT>(v); return s;
    case SQL_C_TYPE_TIMESTAMP: s.kind = Source::Kind::Timestamp; s.ts = readFixed<SQL_TIMESTAMP_STRUCT>(v); return s;
    case SQL_C_GUID: s.kind = Source::Kind::Guid; s.guid = readFixed<SQLGUID>(v); return s;
    case SQL_C_INTERVAL_YEAR: case SQL_C_INTERVAL_MONTH: case SQL_C_INTERVAL_YEAR_TO_MONTH:
      s.kind = Source::Kind::Interval;
      s.iv = readFixed<SQL_INTERVAL_STRUCT>(v);
      s.intervalYearMonth = true;
      return s;
    case SQL_C_INTERVAL_DAY: case SQL_C_INTERVAL_HOUR: case SQL_C_INTERVAL_MINUTE: case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_DAY_TO_HOUR: case SQL_C_INTERVAL_DAY_TO_MINUTE: case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE: case SQL_C_INTERVAL_HOUR_TO_SECOND: case SQL_C_INTERVAL_MINUTE_TO_SECOND:
      s.kind = Source::Kind::Interval;
      s.iv = readFixed<SQL_INTERVAL_STRUCT>(v);
      s.intervalYearMonth = false;
      return s;
    default:
      throwError(sqlstate::kInvalidAppBufferType,
                 formatString("Unsupported parameter C type %d (%s)", static_cast<int>(cType), cTypeName(cType)));
  }
}

// ---- local date/time helpers --------------------------------------------------------------------

bool isLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool validDate(int64_t y, int m, int d) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  return d <= kDays[m - 1] + ((m == 2 && isLeap(y)) ? 1 : 0);
}

bool validTime(int h, int mi, int s, int64_t nanos) {
  return h >= 0 && h < 24 && mi >= 0 && mi < 60 && s >= 0 && s < 60 && nanos >= 0 && nanos <= 999999999;
}

std::string two(int v) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d", v);
  return buf;
}

std::string fmtDate(int64_t y, int m, int d) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04lld-%02d-%02d", static_cast<long long>(y), m, d);
  return buf;
}

/// ".fff" with trailing zeros trimmed but never below `minDigits` (0..9); "" when nothing to show.
std::string fractionText(int64_t nanos, int minDigits) {
  if (minDigits < 0) minDigits = 0;
  if (minDigits > 9) minDigits = 9;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%09lld", static_cast<long long>(nanos));
  std::string digits(buf);
  while (digits.size() > static_cast<size_t>(minDigits) && digits.back() == '0') digits.pop_back();
  return digits.empty() ? std::string() : "." + digits;
}

/// Same trimming rule for a fraction given as text (up to 12 digits from the application).
std::string normalizeFractionText(std::string_view frac, int minDigits) {
  if (minDigits < 0) minDigits = 0;
  std::string digits(frac);
  while (digits.size() > static_cast<size_t>(minDigits) && !digits.empty() && digits.back() == '0') digits.pop_back();
  if (digits.size() < static_cast<size_t>(minDigits)) digits.append(static_cast<size_t>(minDigits) - digits.size(), '0');
  return digits.empty() ? std::string() : "." + digits;
}

std::string fmtTime(int h, int mi, int s, int64_t nanos, int minDigits) {
  return two(h) + ":" + two(mi) + ":" + two(s) + fractionText(nanos, minDigits);
}

/// Parsed pieces of application date/time text (kept as text so up to 12 fraction digits survive).
struct ParsedDateTime {
  bool hasDate = false, hasTime = false;
  int64_t year = 0;
  int month = 0, day = 0, hour = 0, minute = 0, second = 0;
  std::string fraction;  // digits only, may be empty
  std::string zone;      // "+05:30", "UTC", "America/New_York" or empty
};

size_t readDigits(std::string_view s, size_t pos, size_t minCount, size_t maxCount, int64_t& out) {
  size_t n = 0;
  int64_t v = 0;
  while (pos + n < s.size() && n < maxCount && s[pos + n] >= '0' && s[pos + n] <= '9') {
    v = v * 10 + (s[pos + n] - '0');
    ++n;
  }
  if (n < minCount) return 0;
  out = v;
  return n;
}

/// Parses "HH:MM[:SS[.f...]]" at `pos`; returns the position after it, or npos when malformed.
size_t parseTimePart(std::string_view s, size_t pos, ParsedDateTime& out) {
  int64_t h = 0, mi = 0, sec = 0;
  size_t n = readDigits(s, pos, 1, 2, h);
  if (!n || pos + n >= s.size() || s[pos + n] != ':') return std::string_view::npos;
  pos += n + 1;
  n = readDigits(s, pos, 2, 2, mi);
  if (!n) return std::string_view::npos;
  pos += n;
  if (pos < s.size() && s[pos] == ':') {
    n = readDigits(s, pos + 1, 2, 2, sec);
    if (!n) return std::string_view::npos;
    pos += 1 + n;
    if (pos < s.size() && s[pos] == '.') {
      size_t f = pos + 1;
      while (f < s.size() && s[f] >= '0' && s[f] <= '9') ++f;
      const size_t digits = f - (pos + 1);
      if (digits == 0 || digits > 12) return std::string_view::npos;
      out.fraction = std::string(s.substr(pos + 1, digits));
      pos = f;
    }
  }
  out.hasTime = true;
  out.hour = static_cast<int>(h);
  out.minute = static_cast<int>(mi);
  out.second = static_cast<int>(sec);
  return pos;
}

bool parseZoneText(std::string_view z, std::string& out) {
  if (z.empty()) return true;
  if ((z[0] == '+' || z[0] == '-') && (z.size() == 6 || z.size() == 5)) {
    // +HH:MM or +HHMM
    int64_t hh = 0, mm = 0;
    if (readDigits(z, 1, 2, 2, hh) != 2) return false;
    const size_t mmPos = z.size() == 6 ? 4 : 3;
    if (z.size() == 6 && z[3] != ':') return false;
    if (readDigits(z, mmPos, 2, 2, mm) != 2 || hh > 18 || mm > 59) return false;
    out = std::string(1, z[0]) + two(static_cast<int>(hh)) + ":" + two(static_cast<int>(mm));
    return true;
  }
  if (z == "Z" || z == "z") {
    out = "UTC";
    return true;
  }
  // IANA names: letters, digits and _ / + - only. Anything else is not a zone Trino would accept.
  if (!((z[0] >= 'A' && z[0] <= 'Z') || (z[0] >= 'a' && z[0] <= 'z'))) return false;
  for (char c : z) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' ||
                    c == '+' || c == '-';
    if (!ok) return false;
  }
  out = std::string(z);
  return true;
}

/// Accepts "YYYY-MM-DD", "HH:MM:SS[.f]", "YYYY-MM-DD[ T]HH:MM:SS[.f][ zone]" and "HH:MM:SS[.f][zone]".
bool parseDateTimeText(std::string_view raw, ParsedDateTime& out) {
  const std::string_view s = trim(raw);
  out = ParsedDateTime{};
  if (s.empty()) return false;
  size_t pos = 0;
  int64_t y = 0, m = 0, d = 0;
  size_t n = readDigits(s, 0, 4, 6, y);
  const bool looksLikeDate = n && n < s.size() && s[n] == '-';
  if (looksLikeDate) {
    pos = n + 1;
    n = readDigits(s, pos, 1, 2, m);
    if (!n || pos + n >= s.size() || s[pos + n] != '-') return false;
    pos += n + 1;
    n = readDigits(s, pos, 1, 2, d);
    if (!n) return false;
    pos += n;
    if (!validDate(y, static_cast<int>(m), static_cast<int>(d))) return false;
    out.hasDate = true;
    out.year = y;
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(d);
    if (pos == s.size()) return true;
    if (s[pos] != ' ' && s[pos] != 'T' && s[pos] != 't') return false;
    ++pos;
    while (pos < s.size() && s[pos] == ' ') ++pos;
  }
  pos = parseTimePart(s, pos, out);
  if (pos == std::string_view::npos) return false;
  std::string_view rest = trim(s.substr(pos));
  if (!rest.empty() && !parseZoneText(rest, out.zone)) return false;
  return validTime(out.hour, out.minute, out.second, 0);
}

std::string dateText(const ParsedDateTime& p) { return fmtDate(p.year, p.month, p.day); }

std::string timeText(const ParsedDateTime& p, int minDigits) {
  return two(p.hour) + ":" + two(p.minute) + ":" + two(p.second) + normalizeFractionText(p.fraction, minDigits);
}

// ---- source -> canonical text / decimal ---------------------------------------------------------

std::string guidText(const SQLGUID& g) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1),
                static_cast<unsigned>(g.Data2), static_cast<unsigned>(g.Data3), g.Data4[0], g.Data4[1], g.Data4[2],
                g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

/// Canonical Trino interval text: "[-]Y-M" or "[-]D HH:MM:SS[.fff]" (fields are normalised so that
/// SQL_IS_HOUR{hour=30} becomes "1 06:00:00", which is what Trino's literal grammar accepts).
std::string intervalText(const SQL_INTERVAL_STRUCT& iv, bool yearMonth) {
  const bool negative = iv.interval_sign == SQL_TRUE;
  std::string out = negative ? "-" : "";
  if (yearMonth) {
    const uint64_t months = static_cast<uint64_t>(iv.intval.year_month.year) * 12u + iv.intval.year_month.month;
    out += std::to_string(months / 12) + "-" + std::to_string(months % 12);
    return out;
  }
  const auto& ds = iv.intval.day_second;
  // fraction is expressed in the interval seconds precision (ODBC, not nanoseconds like
  // SQL_TIMESTAMP_STRUCT); Trino intervals keep milliseconds.
  uint64_t fractionLimit = 1;
  for (int p = 0; p < kIntervalSecondsPrecision; ++p) fractionLimit *= 10u;
  if (ds.fraction >= fractionLimit) {
    throwError(sqlstate::kIntervalOverflow,
               formatString("Interval field overflow: fraction %u exceeds the interval seconds precision %d",
                            static_cast<unsigned>(ds.fraction), kIntervalSecondsPrecision));
  }
  uint64_t fractionMillis = ds.fraction;
  for (int p = kIntervalSecondsPrecision; p > 3; --p) fractionMillis /= 10u;
  for (int p = kIntervalSecondsPrecision; p < 3; ++p) fractionMillis *= 10u;
  uint64_t totalMillis = ((static_cast<uint64_t>(ds.day) * 24u + ds.hour) * 60u + ds.minute) * 60u + ds.second;
  totalMillis = totalMillis * 1000u + fractionMillis;
  const uint64_t millis = totalMillis % 1000u;
  uint64_t secs = totalMillis / 1000u;
  const uint64_t days = secs / 86400u;
  secs %= 86400u;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%llu %02llu:%02llu:%02llu", static_cast<unsigned long long>(days),
                static_cast<unsigned long long>(secs / 3600u), static_cast<unsigned long long>((secs / 60u) % 60u),
                static_cast<unsigned long long>(secs % 60u));
  out += buf;
  if (millis) {
    std::snprintf(buf, sizeof buf, ".%03llu", static_cast<unsigned long long>(millis));
    out += buf;
  }
  return out;
}

void validateDateStruct(const SQL_DATE_STRUCT& d) {
  if (!validDate(d.year, d.month, d.day)) {
    throwError(sqlstate::kDatetimeOverflow, formatString("Invalid date %d-%d-%d in parameter", static_cast<int>(d.year),
                                                         static_cast<int>(d.month), static_cast<int>(d.day)));
  }
}

void validateTimeStruct(const SQL_TIME_STRUCT& t) {
  if (!validTime(t.hour, t.minute, t.second, 0)) {
    throwError(sqlstate::kDatetimeOverflow, formatString("Invalid time %d:%d:%d in parameter", static_cast<int>(t.hour),
                                                         static_cast<int>(t.minute), static_cast<int>(t.second)));
  }
}

void validateTimestampStruct(const SQL_TIMESTAMP_STRUCT& t) {
  if (!validDate(t.year, t.month, t.day) || !validTime(t.hour, t.minute, t.second, t.fraction)) {
    throwError(sqlstate::kDatetimeOverflow,
               formatString("Invalid timestamp %d-%d-%d %d:%d:%d.%u in parameter", static_cast<int>(t.year),
                            static_cast<int>(t.month), static_cast<int>(t.day), static_cast<int>(t.hour),
                            static_cast<int>(t.minute), static_cast<int>(t.second), static_cast<unsigned>(t.fraction)));
  }
}

/// Text form of any source, the way ODBC defines C -> SQL_CHAR conversions (bit -> "1"/"0", binary -> hex).
std::string sourceToText(const Source& s) {
  switch (s.kind) {
    case Source::Kind::Text: return s.text;
    case Source::Kind::Binary: return hexEncode(s.bytes);
    case Source::Kind::Int: return std::to_string(s.i);
    case Source::Kind::UInt: return std::to_string(s.u);
    case Source::Kind::Double: return s.isFloat ? floatToString(static_cast<float>(s.d)) : doubleToString(s.d);
    case Source::Kind::Decimal: return s.dec.toString();
    case Source::Kind::Bool: return s.b ? "1" : "0";
    case Source::Kind::Date: validateDateStruct(s.date); return fmtDate(s.date.year, s.date.month, s.date.day);
    case Source::Kind::Time: validateTimeStruct(s.time); return fmtTime(s.time.hour, s.time.minute, s.time.second, 0, 0);
    case Source::Kind::Timestamp:
      validateTimestampStruct(s.ts);
      return fmtDate(s.ts.year, s.ts.month, s.ts.day) + " " + fmtTime(s.ts.hour, s.ts.minute, s.ts.second, s.ts.fraction, 0);
    case Source::Kind::Interval: return intervalText(s.iv, s.intervalYearMonth);
    case Source::Kind::Guid: return guidText(s.guid);
  }
  return std::string();
}

/// Exact decimal view of a numeric-ish source. Text must be a numeric literal (22018), doubles finite (22003).
Decimal sourceToDecimal(const Source& s, const Target& t) {
  Decimal d;
  switch (s.kind) {
    case Source::Kind::Text:
      if (!parseDecimal(s.text, d)) throwInvalidValue(s.text, t.typeName);
      return d;
    case Source::Kind::Int: return decimalFromInt64(s.i);
    case Source::Kind::UInt:
      d.digits = std::to_string(s.u);
      return d;
    case Source::Kind::Double:
      if (!std::isfinite(s.d)) throwOutOfRange(doubleToString(s.d), t.typeName);
      return s.isFloat ? decimalFromDouble(static_cast<double>(static_cast<float>(s.d))) : decimalFromDouble(s.d);
    case Source::Kind::Decimal: return s.dec;
    case Source::Kind::Bool: return decimalFromInt64(s.b ? 1 : 0);
    default: throwRestricted(s.cType, t.typeName);
  }
}

// ---- renderers ---------------------------------------------------------------------------------

std::string typed(const char* trinoType, std::string_view text) {
  return std::string(trinoType) + " " + sqlQuoteLiteral(text);
}

std::string renderInteger(const Source& s, const Target& t) {
  const Decimal d = sourceToDecimal(s, t);
  int64_t value = 0;
  bool lost = false;
  if (!decimalToInt64(d, value, &lost)) throwOutOfRange(d.toString(), t.typeName);
  if (lost) {
    // A non-integral value cannot be sent to an integer parameter without silently changing it.
    // Appendix D (C to SQL: Numeric / Character): truncated fractional digits are 22001, whatever
    // the source; 22003 stays for lost whole digits and 22018 for text that is not a number.
    const std::string shown = d.toString().substr(0, 64);
    throwError(sqlstate::kStringRightTruncation,
               formatString("Fractional truncation: %s cannot be sent as %s without dropping its fraction",
                            shown.c_str(), t.typeName.c_str()));
  }
  int64_t lo = std::numeric_limits<int64_t>::min(), hi = std::numeric_limits<int64_t>::max();
  const char* typeLiteral = nullptr;
  switch (t.category) {
    case TypeCategory::TinyInt: lo = -128; hi = 127; typeLiteral = "TINYINT"; break;
    case TypeCategory::SmallInt: lo = -32768; hi = 32767; typeLiteral = "SMALLINT"; break;
    case TypeCategory::Integer: lo = std::numeric_limits<int32_t>::min(); hi = std::numeric_limits<int32_t>::max(); break;
    default: break;  // bigint: full int64 range
  }
  if (value < lo || value > hi) throwOutOfRange(d.toString(), t.typeName);
  const std::string text = std::to_string(value);
  // integer/bigint literals are plain (Trino types them by magnitude and widens integer -> bigint);
  // tinyint/smallint need the explicit form because Trino never narrows implicitly.
  return typeLiteral ? typed(typeLiteral, text) : text;
}

std::string renderFloating(const Source& s, const Target& t) {
  const bool real = t.category == TypeCategory::Real;
  const char* literal = real ? "REAL" : "DOUBLE";
  if (s.kind == Source::Kind::Text) {
    const std::string_view text = trim(s.text);
    if (iequals(text, "nan")) return typed(literal, "NaN");
    if (iequals(text, "infinity") || iequals(text, "+infinity") || iequals(text, "inf") || iequals(text, "+inf")) {
      return typed(literal, "Infinity");
    }
    if (iequals(text, "-infinity") || iequals(text, "-inf")) return typed(literal, "-Infinity");
    Decimal d;
    if (!parseDecimal(text, d)) throwInvalidValue(s.text, t.typeName);
    return typed(literal, d.toString());
  }
  if (s.kind == Source::Kind::Double) {
    if (std::isnan(s.d)) return typed(literal, "NaN");
    if (std::isinf(s.d)) return typed(literal, s.d < 0 ? "-Infinity" : "Infinity");
    if (real && std::fabs(s.d) > static_cast<double>(std::numeric_limits<float>::max())) {
      throwOutOfRange(doubleToString(s.d), t.typeName);
    }
    return typed(literal, s.isFloat ? floatToString(static_cast<float>(s.d)) : doubleToString(s.d));
  }
  return typed(literal, sourceToDecimal(s, t).toString());
}

std::string renderDecimal(const Source& s, const Target& t) {
  Decimal d = sourceToDecimal(s, t);
  const int maxScale = t.scale >= 0 ? t.scale : kTrinoMaxDecimalPrecision;
  if (t.scale >= 0 || d.scale > maxScale) {
    // Applications routinely pass doubles like 0.30000000000000004 for a decimal(10,2) parameter:
    // rounding to the declared scale is the expected behaviour (SQL Server / Oracle drivers do the same).
    if (rescaleDecimal(d, maxScale, /*round=*/true)) {
      TRINO_LOG_DEBUG(kLog, "decimal parameter rounded to scale %d for type %s", maxScale, t.typeName.c_str());
    }
  }
  const int precision = t.precision > 0 ? t.precision : kTrinoMaxDecimalPrecision;
  if (!d.isZero() && d.digits.size() > static_cast<size_t>(precision)) throwOutOfRange(d.toString(), t.typeName);
  return typed("DECIMAL", d.toString());
}

std::string renderBoolean(const Source& s, const Target& t) {
  bool value = false;
  switch (s.kind) {
    case Source::Kind::Bool: value = s.b; break;
    case Source::Kind::Text: {
      if (!parseBool(trim(s.text), &value)) throwInvalidValue(s.text, t.typeName);
      break;
    }
    case Source::Kind::Int: case Source::Kind::UInt: case Source::Kind::Double: case Source::Kind::Decimal: {
      const Decimal d = sourceToDecimal(s, t);
      int64_t iv = 0;
      bool lost = false;
      if (!decimalToInt64(d, iv, &lost) || lost || iv < 0 || iv > 1) throwOutOfRange(d.toString(), t.typeName);
      value = iv == 1;
      break;
    }
    default: throwRestricted(s.cType, t.typeName);
  }
  return value ? "true" : "false";
}

bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string renderBinary(const Source& s, const Target& t) {
  if (s.kind == Source::Kind::Binary) return "X" + sqlQuoteLiteral(hexEncode(s.bytes));
  if (s.kind == Source::Kind::Text) {
    // ODBC char -> binary: the text must be hexadecimal digits, two per byte.
    const std::string_view text = trim(s.text);
    if (text.size() % 2 != 0) throwInvalidValue(s.text, t.typeName);
    for (char c : text) {
      if (!isHexDigit(c)) throwInvalidValue(s.text, t.typeName);
    }
    return "X" + sqlQuoteLiteral(toUpper(text));
  }
  throwRestricted(s.cType, t.typeName);
}

std::string renderDate(const Source& s, const Target& t) {
  switch (s.kind) {
    case Source::Kind::Date:
      validateDateStruct(s.date);
      return typed("DATE", fmtDate(s.date.year, s.date.month, s.date.day));
    case Source::Kind::Timestamp:
      validateTimestampStruct(s.ts);
      if (s.ts.hour || s.ts.minute || s.ts.second || s.ts.fraction) {
        throwError(sqlstate::kDatetimeOverflow, "Timestamp parameter with a non-zero time part cannot be sent as DATE");
      }
      return typed("DATE", fmtDate(s.ts.year, s.ts.month, s.ts.day));
    case Source::Kind::Text: {
      ParsedDateTime p;
      if (!parseDateTimeText(s.text, p) || !p.hasDate) throwInvalidValue(s.text, t.typeName);
      if (p.hasTime && (p.hour || p.minute || p.second || (!p.fraction.empty() && p.fraction.find_first_not_of('0') != std::string::npos))) {
        throwError(sqlstate::kDatetimeOverflow, "Timestamp parameter with a non-zero time part cannot be sent as DATE");
      }
      return typed("DATE", dateText(p));
    }
    default: throwRestricted(s.cType, t.typeName);
  }
}

std::string renderTime(const Source& s, const Target& t) {
  const int minDigits = t.fractionDigits < 0 ? 0 : t.fractionDigits;
  switch (s.kind) {
    case Source::Kind::Time:
      validateTimeStruct(s.time);
      return typed("TIME", fmtTime(s.time.hour, s.time.minute, s.time.second, 0, minDigits));
    case Source::Kind::Timestamp:
      // Trino TIME carries fractional seconds, so unlike the ODBC minimum we keep the fraction.
      validateTimestampStruct(s.ts);
      return typed("TIME", fmtTime(s.ts.hour, s.ts.minute, s.ts.second, s.ts.fraction, minDigits));
    case Source::Kind::Text: {
      ParsedDateTime p;
      if (!parseDateTimeText(s.text, p) || !p.hasTime) throwInvalidValue(s.text, t.typeName);
      std::string text = timeText(p, minDigits);
      if (!p.zone.empty()) {
        // Only fixed offsets make sense for a TIME literal; a region name needs a date to resolve.
        if (p.zone[0] != '+' && p.zone[0] != '-') throwInvalidValue(s.text, t.typeName);
        text += p.zone;
      }
      return typed("TIME", text);
    }
    default: throwRestricted(s.cType, t.typeName);
  }
}

/// "YYYY-MM-DD" of today in the session time zone (ODBC time -> timestamp takes the current date).
std::string currentDateText(const ConvertOptions& opts) {
  const CivilDate today = currentCivilDate(opts.sessionTimeZone);
  return fmtDate(today.year, today.month, today.day);
}

std::string renderTimestamp(const Source& s, const Target& t, const ConvertOptions& opts) {
  const int minDigits = t.fractionDigits < 0 ? 3 : t.fractionDigits;
  switch (s.kind) {
    case Source::Kind::Time:
      // Appendix D (C to SQL: Time): the date fields become the current date, the fraction zero.
      validateTimeStruct(s.time);
      return typed("TIMESTAMP", currentDateText(opts) + " " + fmtTime(s.time.hour, s.time.minute, s.time.second, 0, minDigits));
    case Source::Kind::Timestamp:
      validateTimestampStruct(s.ts);
      return typed("TIMESTAMP", fmtDate(s.ts.year, s.ts.month, s.ts.day) + " " +
                                    fmtTime(s.ts.hour, s.ts.minute, s.ts.second, s.ts.fraction, minDigits));
    case Source::Kind::Date:
      validateDateStruct(s.date);
      return typed("TIMESTAMP", fmtDate(s.date.year, s.date.month, s.date.day) + " " + fmtTime(0, 0, 0, 0, minDigits));
    case Source::Kind::Text: {
      ParsedDateTime p;
      if (!parseDateTimeText(s.text, p)) throwInvalidValue(s.text, t.typeName);
      // A time-only literal is a valid time-value for a timestamp target: today's date is prefixed.
      std::string text = (p.hasDate ? dateText(p) : currentDateText(opts)) + " " +
                         (p.hasTime ? timeText(p, minDigits) : fmtTime(0, 0, 0, 0, minDigits));
      if (!p.zone.empty()) text += (p.zone[0] == '+' || p.zone[0] == '-') ? p.zone : " " + p.zone;
      return typed("TIMESTAMP", text);
    }
    default: throwRestricted(s.cType, t.typeName);
  }
}

/// Single-field interval qualifier for numeric sources (ODBC allows numeric -> single-field interval only).
const char* singleIntervalField(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_INTERVAL_YEAR: return "YEAR";
    case SQL_INTERVAL_MONTH: return "MONTH";
    case SQL_INTERVAL_DAY: return "DAY";
    case SQL_INTERVAL_HOUR: return "HOUR";
    case SQL_INTERVAL_MINUTE: return "MINUTE";
    case SQL_INTERVAL_SECOND: return "SECOND";
    default: return nullptr;
  }
}

bool validIntervalText(std::string_view text, bool yearMonth) {
  size_t pos = 0;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
  int64_t v = 0;
  size_t n = readDigits(text, pos, 1, 18, v);
  if (!n) return false;
  pos += n;
  if (pos == text.size()) return true;  // a bare count is still a valid single-field literal
  if (yearMonth) {
    if (text[pos] != '-') return false;
    n = readDigits(text, pos + 1, 1, 18, v);
    return n && pos + 1 + n == text.size();
  }
  if (text[pos] != ' ') return false;
  ParsedDateTime p;
  const size_t end = parseTimePart(text, pos + 1, p);
  return end == text.size() && p.minute < 60 && p.second < 60;
}

std::string renderInterval(const Source& s, const Target& t) {
  const bool yearMonth = t.category == TypeCategory::IntervalYearToMonth;
  const char* qualifier = yearMonth ? "YEAR TO MONTH" : "DAY TO SECOND";
  switch (s.kind) {
    case Source::Kind::Interval:
      if (s.intervalYearMonth != yearMonth) throwRestricted(s.cType, t.typeName);
      return "INTERVAL " + sqlQuoteLiteral(intervalText(s.iv, yearMonth)) + " " + qualifier;
    case Source::Kind::Text: {
      const std::string_view text = trim(s.text);
      if (!validIntervalText(text, yearMonth)) throwInvalidValue(s.text, t.typeName);
      // A bare number only has a meaning when the bound type names a single field.
      if (text.find_first_of(" -", 1) == std::string_view::npos) {
        const char* field = singleIntervalField(t.sqlType);
        if (!field) throwInvalidValue(s.text, t.typeName);
        return "INTERVAL " + sqlQuoteLiteral(text) + " " + field;
      }
      return "INTERVAL " + sqlQuoteLiteral(text) + " " + qualifier;
    }
    case Source::Kind::Int: case Source::Kind::UInt: case Source::Kind::Decimal: case Source::Kind::Double: {
      const char* field = singleIntervalField(t.sqlType);
      if (!field) throwRestricted(s.cType, t.typeName);
      const Decimal d = sourceToDecimal(s, t);
      int64_t v = 0;
      bool lost = false;
      if (!decimalToInt64(d, v, &lost) || lost) throwOutOfRange(d.toString(), t.typeName);
      return "INTERVAL " + sqlQuoteLiteral(std::to_string(v)) + " " + field;
    }
    default: throwRestricted(s.cType, t.typeName);
  }
}

bool validUuidText(std::string_view text) {
  if (text.size() != 36) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != '-' : !isHexDigit(text[i])) return false;
  }
  return true;
}

std::string renderUuid(const Source& s, const Target& t) {
  switch (s.kind) {
    case Source::Kind::Guid: return typed("UUID", guidText(s.guid));
    case Source::Kind::Text: {
      const std::string_view text = trim(s.text);
      if (!validUuidText(text)) throwInvalidValue(s.text, t.typeName);
      return typed("UUID", toLower(text));
    }
    case Source::Kind::Binary: {
      if (s.bytes.size() != 16) throwInvalidValue(hexEncode(s.bytes), t.typeName);
      const std::string hex = hexEncode(s.bytes, /*upper=*/false);
      return typed("UUID", hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) +
                               "-" + hex.substr(20, 12));
    }
    default: throwRestricted(s.cType, t.typeName);
  }
}

std::string renderJson(const Source& s, const Target& t) {
  switch (s.kind) {
    case Source::Kind::Text: case Source::Kind::Int: case Source::Kind::UInt: case Source::Kind::Double:
    case Source::Kind::Decimal:
      return typed("JSON", sourceToText(s));
    case Source::Kind::Bool: return typed("JSON", s.b ? "true" : "false");
    default: throwRestricted(s.cType, t.typeName);
  }
}

std::string renderComplex(const Source& s, const Target& t) {
  // array/map/row parameters arrive as JSON text; Trino casts json -> array/map/row natively.
  if (s.kind != Source::Kind::Text) throwRestricted(s.cType, t.typeName);
  return "CAST(" + typed("JSON", s.text) + " AS " + t.typeName + ")";
}

std::string renderOther(const Source& s, const Target& t) {
  // ipaddress and any exotic type: every one of them has a varchar cast.
  if (s.kind == Source::Kind::Binary) throwRestricted(s.cType, t.typeName);
  return "CAST(" + sqlQuoteLiteral(sourceToText(s)) + " AS " + t.typeName + ")";
}

}  // namespace

std::string renderParameterLiteral(const ParamValue& value, const ConvertOptions& opts,
                                   const std::optional<TrinoType>& serverType) {
  if (value.isDefault) {
    throwError(sqlstate::kOptionalFeatureNotImplemented, "SQL_DEFAULT_PARAM is not supported: Trino has no parameter defaults");
  }

  // Resolve the C type first: SQL_C_DEFAULT follows the bound SQL type (ODBC Appendix D), and when
  // nothing was bound the server type decides between char and binary transfer.
  SQLSMALLINT cType = normalizeCType(value.cType);
  if (cType == SQL_C_DEFAULT) {
    if (value.sqlType != SQL_UNKNOWN_TYPE && categoryForSqlType(value.sqlType) != TypeCategory::Unknown) {
      cType = defaultCTypeFor(value.sqlType);
    } else if (serverType && serverType->category == TypeCategory::Varbinary) {
      cType = SQL_C_BINARY;
    } else {
      cType = SQL_C_CHAR;
    }
    cType = normalizeCType(cType);
    if (cType == SQL_C_DEFAULT) cType = SQL_C_CHAR;
  }

  const Target target = resolveTarget(value, cType, serverType);

  const bool dae = value.daeData != nullptr;
  if (value.isNull || (!dae && value.length == SQL_NULL_DATA)) return "CAST(NULL AS " + target.typeName + ")";
  if (!dae && (value.length == SQL_DATA_AT_EXEC || value.length <= SQL_LEN_DATA_AT_EXEC_OFFSET)) {
    throwError(sqlstate::kFunctionSequence, "Data-at-execution parameter has not received its data");
  }
  if (!dae && !value.data) throwError(sqlstate::kInvalidNullPointer, "Parameter value pointer is null");

  const Source source = readSource(value, cType, opts);

  std::string literal;
  switch (target.category) {
    case TypeCategory::Varchar: case TypeCategory::Char: literal = sqlQuoteLiteral(sourceToText(source)); break;
    case TypeCategory::Boolean: literal = renderBoolean(source, target); break;
    case TypeCategory::TinyInt: case TypeCategory::SmallInt: case TypeCategory::Integer: case TypeCategory::BigInt:
      literal = renderInteger(source, target);
      break;
    case TypeCategory::Real: case TypeCategory::Double: literal = renderFloating(source, target); break;
    case TypeCategory::Decimal: literal = renderDecimal(source, target); break;
    case TypeCategory::Varbinary: literal = renderBinary(source, target); break;
    case TypeCategory::Date: literal = renderDate(source, target); break;
    case TypeCategory::Time: case TypeCategory::TimeWithTimeZone: literal = renderTime(source, target); break;
    case TypeCategory::Timestamp: case TypeCategory::TimestampWithTimeZone: literal = renderTimestamp(source, target, opts); break;
    case TypeCategory::IntervalYearToMonth: case TypeCategory::IntervalDayToSecond: literal = renderInterval(source, target); break;
    case TypeCategory::Uuid: literal = renderUuid(source, target); break;
    case TypeCategory::Json: literal = renderJson(source, target); break;
    case TypeCategory::Array: case TypeCategory::Map: case TypeCategory::Row: literal = renderComplex(source, target); break;
    case TypeCategory::IpAddress: case TypeCategory::Unknown: literal = renderOther(source, target); break;
  }
  if (TRINO_LOG_ENABLED(LogLevel::Trace)) {
    TRINO_LOG_TRACE(kLog, "parameter %s -> %s: %.*s%s", cTypeName(cType), target.typeName.c_str(),
                    static_cast<int>(literal.size() > 200 ? 200 : literal.size()), literal.c_str(),
                    literal.size() > 200 ? "..." : "");
  }
  return literal;
}

}  // namespace trino_odbc
