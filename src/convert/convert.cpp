// Column value -> application C buffer conversions (SQLGetData / SQLFetch) implementing the ODBC
// conversion matrix for the five StorageKinds (ARCHITECTURE.md §8).
//
// Layout of this file:
//   1. small helpers (diagnostics, chunk bookkeeping, category predicates)
//   2. text rendering (cellToText and the zero-copy variant used on the fetch path)
//   3. character / binary targets with SQLGetData chunking
//   4. numeric targets (integers, floats, SQL_C_BIT, SQL_C_NUMERIC)
//   5. temporal targets (date / time / timestamp)
//   6. interval and GUID targets
//   7. convertToC dispatcher
#include "convert/convert.h"

#include "convert/numeric.h"
#include "convert/temporal.h"
#include "types/odbc_types.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/tz.h"
#include "util/unicode.h"

#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "convert";
constexpr int64_t kNanosPerSecond = 1000000000LL;
// ODBC default interval seconds precision when neither the ARD nor the column supplies one.
constexpr int kDefaultIntervalSecondsPrecision = 6;
constexpr SQLSMALLINT kDefaultNumericPrecision = 38;

// ---------------------------------------------------------------------------------------------
// 1. helpers
// ---------------------------------------------------------------------------------------------

/// Everything one conversion call needs; passed by reference to keep signatures short.
struct Ctx {
  const ResultColumn& col;
  const Column& data;
  size_t row;
  const ConvertTarget& target;
  const ConvertOptions& opts;
  size_t* chunkOffset;
  Diag& diag;
  SQLLEN rowNumber;
  SQLINTEGER columnNumber;
  SQLSMALLINT cType;

  SQLRETURN error(const char* state, const std::string& msg) {
    TRINO_LOG_DEBUG(kLog, "row %lld col %d: %s %s", static_cast<long long>(rowNumber),
                    static_cast<int>(columnNumber), state, msg.c_str());
    return diag.addError(state, msg, 0, rowNumber, columnNumber);
  }
  SQLRETURN warning(const char* state, const std::string& msg) {
    return diag.addWarning(state, msg, 0, rowNumber, columnNumber);
  }
  SQLRETURN restricted() {
    return error(sqlstate::kRestrictedDataType,
                 std::string("Conversion from ") + col.type.toString() + " to " + cTypeName(cType) +
                     " is not supported");
  }
  void setIndicator(SQLLEN v) const {
    if (target.strLenOrInd) *target.strLenOrInd = v;
  }
  void markConsumed() const {
    if (chunkOffset) *chunkOffset = SIZE_MAX;
  }
};

/// Character-like categories follow the ODBC "character SQL type" rules: any target is attempted
/// by parsing the text, and unparsable text yields 22018 rather than 07006.
bool isCharLike(TypeCategory c) {
  switch (c) {
    case TypeCategory::Varchar:
    case TypeCategory::Char:
    case TypeCategory::Json:
    case TypeCategory::Array:
    case TypeCategory::Map:
    case TypeCategory::Row:
    case TypeCategory::IpAddress:
    case TypeCategory::Unknown:
      return true;
    default:
      return false;
  }
}

bool isExactNumericCategory(TypeCategory c) {
  switch (c) {
    case TypeCategory::TinyInt:
    case TypeCategory::SmallInt:
    case TypeCategory::Integer:
    case TypeCategory::BigInt:
    case TypeCategory::Decimal:
      return true;
    default:
      return false;
  }
}

bool isYearMonthCType(SQLSMALLINT t) {
  return t == SQL_C_INTERVAL_YEAR || t == SQL_C_INTERVAL_MONTH || t == SQL_C_INTERVAL_YEAR_TO_MONTH;
}

bool isDaySecondCType(SQLSMALLINT t) {
  switch (t) {
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_DAY_TO_HOUR:
    case SQL_C_INTERVAL_DAY_TO_MINUTE:
    case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    case SQL_C_INTERVAL_MINUTE_TO_SECOND:
      return true;
    default:
      return false;
  }
}

bool isIntervalCType(SQLSMALLINT t) { return isYearMonthCType(t) || isDaySecondCType(t); }

/// Legacy ODBC 2 C types map onto their ODBC 3 equivalents (same struct layout).
SQLSMALLINT normalizeLegacyCType(SQLSMALLINT t) {
  switch (t) {
    case SQL_C_DATE: return SQL_C_TYPE_DATE;
    case SQL_C_TIME: return SQL_C_TYPE_TIME;
    case SQL_C_TIMESTAMP: return SQL_C_TYPE_TIMESTAMP;
    default: return t;
  }
}

int64_t pow10i(int n) {
  int64_t v = 1;
  while (n-- > 0) v *= 10;
  return v;
}

/// Writes a fixed-size value, sets the indicator and marks the column consumed. `rc` is the
/// conversion outcome (SQL_ERROR writes nothing).
template <typename T>
SQLRETURN emitFixed(Ctx& c, const T& value, SQLRETURN rc) {
  if (rc == SQL_ERROR) return rc;
  if (c.target.buffer) std::memcpy(c.target.buffer, &value, sizeof(T));
  c.setIndicator(static_cast<SQLLEN>(sizeof(T)));
  c.markConsumed();
  return rc;
}

/// Raw text of the cell as stored (String storage only).
std::string_view rawText(const Ctx& c) { return c.data.getBytes(c.row); }

std::string quoted(std::string_view s) {
  // Keep diagnostics readable for huge cells.
  constexpr size_t kMax = 64;
  std::string out = "'";
  if (s.size() > kMax) {
    out.append(s.data(), utf8SafeCut(s, kMax));
    out += "...";
  } else {
    out.append(s.data(), s.size());
  }
  out += "'";
  return out;
}

// ---------------------------------------------------------------------------------------------
// 2. text rendering
// ---------------------------------------------------------------------------------------------

/// Appends the fractional digits beyond the 9th of a Trino temporal text (picosecond precision),
/// which CivilTime cannot carry. `precision` is the digit count the text had.
void appendSubNanoDigits(std::string_view raw, int precision, std::string& out) {
  if (precision <= 9) return;
  const size_t dot = raw.find('.');
  if (dot == std::string_view::npos) return;
  size_t pos = dot + 1;
  int seen = 0;
  while (pos < raw.size() && seen < 9 && raw[pos] >= '0' && raw[pos] <= '9') {
    ++pos;
    ++seen;
  }
  while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
    out.push_back(raw[pos]);
    ++pos;
  }
}

/// True when the text carries non-zero digits beyond nanosecond precision.
bool hasNonZeroSubNanoDigits(std::string_view raw) {
  const size_t dot = raw.find('.');
  if (dot == std::string_view::npos) return false;
  size_t pos = dot + 1;
  int seen = 0;
  while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
    if (seen >= 9 && raw[pos] != '0') return true;
    ++pos;
    ++seen;
  }
  return false;
}

/// Adds the "unknown zone" warning unless an identical record already exists in the diag area
/// (the area is cleared per API call, so this is "once per call" — a fetch of a large rowset with
/// hundreds of such cells produces one record instead of hundreds).
SQLRETURN warnOnce(Diag* diag, SQLLEN rowNumber, SQLINTEGER columnNumber, const std::string& msg) {
  if (!diag) return SQL_SUCCESS;
  for (const DiagRecord& rec : diag->records()) {
    if (rec.sqlstate == sqlstate::kGeneralWarning && rec.message.find(msg) != std::string::npos) {
      return SQL_SUCCESS_WITH_INFO;
    }
  }
  return diag->addWarning(sqlstate::kGeneralWarning, msg, 0, rowNumber, columnNumber);
}

SQLRETURN warnUnknownZoneOnce(Diag* diag, SQLLEN rowNumber, SQLINTEGER columnNumber, std::string_view zone) {
  return warnOnce(diag, rowNumber, columnNumber,
                  "Time zone '" + std::string(zone) + "' is unknown; value returned as wall-clock time");
}

/// Session modes whose target zone cannot be resolved deliver UTC instead (same 01000 path).
SQLRETURN warnUnknownSessionZoneOnce(Diag* diag, SQLLEN rowNumber, SQLINTEGER columnNumber, std::string_view zone) {
  return warnOnce(diag, rowNumber, columnNumber,
                  "Session time zone '" + std::string(zone) + "' is unknown; value returned in UTC");
}

/// Applies the TimestampWithTimeZoneMode to a parsed zoned timestamp (String/WallClock: untouched).
SQLRETURN applyTimestampTzMode(ParsedTimestamp& ts, const ConvertOptions& opts, Diag* diag, SQLLEN rowNumber,
                               SQLINTEGER columnNumber) {
  if (!ts.hasZone) return SQL_SUCCESS;
  switch (opts.timestampTz) {
    case TimestampTzMode::Utc:
      if (!timestampToUtc(ts)) return warnUnknownZoneOnce(diag, rowNumber, columnNumber, ts.zoneName);
      return SQL_SUCCESS;
    case TimestampTzMode::Session: {
      const std::string valueZone = ts.zoneName;
      switch (timestampToZone(ts, opts.sessionTimeZone)) {
        case ZoneConversion::Ok: return SQL_SUCCESS;
        case ZoneConversion::UnknownValueZone: return warnUnknownZoneOnce(diag, rowNumber, columnNumber, valueZone);
        case ZoneConversion::UnknownTargetZone:
          return warnUnknownSessionZoneOnce(diag, rowNumber, columnNumber, opts.sessionTimeZone);
      }
      return SQL_SUCCESS;
    }
    case TimestampTzMode::WallClock:
    case TimestampTzMode::String:
      break;
  }
  return SQL_SUCCESS;  // zone dropped
}

/// Applies the TimeWithTimeZoneMode to a parsed time-of-day with offset (String/WallClock: untouched).
SQLRETURN applyTimeTzMode(CivilTime& t, bool hasOffset, int32_t offset, const ConvertOptions& opts, Diag* diag,
                          SQLLEN rowNumber, SQLINTEGER columnNumber) {
  if (!hasOffset) return SQL_SUCCESS;
  if (opts.timeTz == TimeTzMode::Utc) {
    timeToZone(t, offset, std::string_view());  // empty zone = shift to UTC
    return SQL_SUCCESS;
  }
  if (opts.timeTz == TimeTzMode::Session && !timeToZone(t, offset, opts.sessionTimeZone)) {
    return warnUnknownSessionZoneOnce(diag, rowNumber, columnNumber, opts.sessionTimeZone);
  }
  return SQL_SUCCESS;
}

/// Text of a timestamp-with-time-zone cell for the configured mode (String mode: unchanged).
SQLRETURN renderTimestampTz(std::string_view raw, const ConvertOptions& opts, Diag* diag, SQLLEN rowNumber,
                            SQLINTEGER columnNumber, std::string& out, std::string_view& view) {
  ParsedTimestamp ts;
  if (opts.timestampTz == TimestampTzMode::String || !parseTrinoTimestamp(raw, ts)) {
    view = raw;
    return SQL_SUCCESS;
  }
  const SQLRETURN rc = applyTimestampTzMode(ts, opts, diag, rowNumber, columnNumber);
  out = formatTimestamp(ts.date, ts.time, ts.time.precision > 9 ? 9 : ts.time.precision);
  appendSubNanoDigits(raw, ts.time.precision, out);
  view = out;
  return rc;
}

/// Text of a time-with-time-zone cell for the configured mode.
SQLRETURN renderTimeTz(std::string_view raw, const ConvertOptions& opts, Diag* diag, SQLLEN rowNumber,
                       SQLINTEGER columnNumber, std::string& out, std::string_view& view) {
  CivilTime t;
  int32_t offset = 0;
  bool hasOffset = false;
  if (opts.timeTz == TimeTzMode::String || !parseTrinoTime(raw, t, &offset, &hasOffset)) {
    view = raw;
    return SQL_SUCCESS;
  }
  const SQLRETURN rc = applyTimeTzMode(t, hasOffset, offset, opts, diag, rowNumber, columnNumber);
  out = formatTime(t, t.precision > 9 ? 9 : t.precision);
  appendSubNanoDigits(raw, t.precision, out);
  view = out;
  return rc;
}

/// Renders the cell as text. `view` points either into the page arena (no copy) or into `scratch`.
/// Returns SQL_SUCCESS_WITH_INFO when a zone warning was added (diag may be null).
SQLRETURN renderText(const ResultColumn& col, const Column& data, size_t row, const ConvertOptions& opts,
                     Diag* diag, SQLLEN rowNumber, SQLINTEGER columnNumber, std::string& scratch,
                     std::string_view& view) {
  switch (data.kind()) {
    case StorageKind::Bool:
      view = data.getBool(row) ? std::string_view("1") : std::string_view("0");
      return SQL_SUCCESS;
    case StorageKind::Int64:
      scratch = std::to_string(data.getInt64(row));
      view = scratch;
      return SQL_SUCCESS;
    case StorageKind::Double: {
      const double v = data.getDouble(row);
      // real columns are stored widened to double; the float's shortest text is the faithful one.
      scratch = col.type.category == TypeCategory::Real ? floatToString(static_cast<float>(v)) : doubleToString(v);
      view = scratch;
      return SQL_SUCCESS;
    }
    case StorageKind::Binary:
      scratch = hexEncode(data.getBytes(row));
      view = scratch;
      return SQL_SUCCESS;
    case StorageKind::String: {
      const std::string_view raw = data.getBytes(row);
      switch (col.type.category) {
        case TypeCategory::TimestampWithTimeZone:
          return renderTimestampTz(raw, opts, diag, rowNumber, columnNumber, scratch, view);
        case TypeCategory::TimeWithTimeZone:
          return renderTimeTz(raw, opts, diag, rowNumber, columnNumber, scratch, view);
        default:
          view = raw;
          return SQL_SUCCESS;
      }
    }
  }
  view = std::string_view();
  return SQL_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// 3. character / binary targets (chunked)
// ---------------------------------------------------------------------------------------------

/// Largest prefix of narrow (AnsiEncoding::System) text that ends on a character boundary.
size_t narrowSafeCut(std::string_view s, size_t maxBytes) {
  if (maxBytes >= s.size()) return s.size();
#if defined(TRINO_ODBC_WINDOWS)
  const UINT cp = GetACP();
  if (cp == CP_UTF8) return utf8SafeCut(s, maxBytes);
  // Walk from the start (always a boundary) so DBCS lead/trail pairs are never split.
  size_t pos = 0;
  while (pos < s.size()) {
    const size_t len = IsDBCSLeadByteEx(cp, static_cast<BYTE>(s[pos])) ? 2 : 1;
    if (pos + len > maxBytes) break;
    pos += len;
  }
  return pos;
#else
  return utf8SafeCut(s, maxBytes);
#endif
}

/// Shared chunk delivery for byte-oriented targets (SQL_C_CHAR and SQL_C_BINARY).
/// `unitBytes` is 1; `terminate` adds the NUL; `cut` picks a safe split point.
SQLRETURN deliverBytes(Ctx& c, std::string_view bytes, bool terminate, size_t (*cut)(std::string_view, size_t),
                       SQLRETURN rcSoFar) {
  size_t offset = c.chunkOffset ? *c.chunkOffset : 0;
  if (offset > bytes.size()) offset = bytes.size();
  const std::string_view rest = bytes.substr(offset);
  c.setIndicator(static_cast<SQLLEN>(rest.size()));

  if (!c.target.buffer) {
    // Length-only request (SQLBindCol with a null data pointer): nothing is consumed.
    return rcSoFar;
  }
  const SQLLEN bufLen = c.target.bufferLength < 0 ? 0 : c.target.bufferLength;
  const size_t capacity = static_cast<size_t>(bufLen);
  const size_t dataCap = terminate ? (capacity > 0 ? capacity - 1 : 0) : capacity;
  auto* out = static_cast<unsigned char*>(c.target.buffer);

  size_t n = dataCap == 0 ? 0 : cut(rest, dataCap);
  // A buffer too small for one whole character must still make progress (SQL_C_CHAR is a byte
  // stream), otherwise a SQLGetData loop would never terminate: split the code point.
  if (n == 0 && dataCap > 0 && !rest.empty()) n = std::min(dataCap, rest.size());
  if (n > 0) std::memcpy(out, rest.data(), n);
  if (terminate && capacity > 0) out[n] = 0;

  if (n < rest.size()) {
    if (c.chunkOffset) *c.chunkOffset = offset + n;
    const SQLRETURN rc = c.warning(sqlstate::kStringTruncated, "String data, right truncated");
    return mergeReturnCode(rcSoFar, rc);
  }
  c.markConsumed();
  return rcSoFar;
}

SQLRETURN toChar(Ctx& c, std::string_view utf8, SQLRETURN rcSoFar) {
  if (c.opts.ansi == AnsiEncoding::System) {
    const std::string narrow = utf8ToNarrow(utf8, c.opts.ansi);
    return deliverBytes(c, narrow, true, &narrowSafeCut, rcSoFar);
  }
  return deliverBytes(c, utf8, true, &utf8SafeCut, rcSoFar);
}

SQLRETURN toWChar(Ctx& c, std::string_view utf8, SQLRETURN rcSoFar) {
  size_t offset = c.chunkOffset ? *c.chunkOffset : 0;
  if (offset > utf8.size()) offset = utf8.size();
  const std::string_view rest = utf8.substr(offset);
  const size_t remainingUnits = wideUnitsFor(rest);
  c.setIndicator(static_cast<SQLLEN>(remainingUnits * sizeof(SQLWCHAR)));

  if (!c.target.buffer) return rcSoFar;
  const SQLLEN bufLen = c.target.bufferLength < 0 ? 0 : c.target.bufferLength;
  const size_t capUnits = static_cast<size_t>(bufLen) / sizeof(SQLWCHAR);
  const size_t dataUnits = capUnits > 0 ? capUnits - 1 : 0;
  auto* out = static_cast<SQLWCHAR*>(c.target.buffer);

  size_t consumed = 0;
  // utf8ToWideChunk never splits a surrogate pair, so a chunk boundary is always a code point boundary.
  const size_t written = dataUnits == 0 ? 0 : utf8ToWideChunk(rest, out, dataUnits, &consumed);
  if (capUnits > 0) out[written] = 0;
  if (written == 0 && dataUnits > 0 && !rest.empty()) {
    // One SQLWCHAR unit cannot hold a supplementary character (surrogate pair); refuse rather than
    // loop forever or emit a lone surrogate.
    return c.error(sqlstate::kInvalidStringOrBufferLength,
                   "Buffer too small to hold a single character of the value");
  }

  if (consumed < rest.size()) {
    if (c.chunkOffset) *c.chunkOffset = offset + consumed;
    const SQLRETURN rc = c.warning(sqlstate::kStringTruncated, "String data, right truncated");
    return mergeReturnCode(rcSoFar, rc);
  }
  c.markConsumed();
  return rcSoFar;
}

size_t binaryCut(std::string_view s, size_t maxBytes) { return maxBytes < s.size() ? maxBytes : s.size(); }

/// How the text of a column may be cut when it does not fit a SQL_C_CHAR / SQL_C_WCHAR buffer,
/// decided by the SQL type the application sees (ODBC Appendix D, "SQL to C").
enum class TextFit {
  Stream,    // character / binary SQL types: cut anywhere with 01004, the rest via SQLGetData chunks
  Fraction,  // numeric, datetime and interval types: only fractional digits may go (01004), else 22003
  Whole      // SQL_GUID: the whole value or 22003
};

TextFit textFitFor(const OdbcTypeInfo& info) {
  switch (info.conciseType) {
    case SQL_CHAR: case SQL_VARCHAR: case SQL_LONGVARCHAR:
    case SQL_WCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR:
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY:
      return TextFit::Stream;
    case SQL_BIT: case SQL_TINYINT: case SQL_SMALLINT: case SQL_INTEGER: case SQL_BIGINT:
    case SQL_DECIMAL: case SQL_NUMERIC: case SQL_REAL: case SQL_FLOAT: case SQL_DOUBLE:
    case SQL_TYPE_DATE: case SQL_TYPE_TIME: case SQL_TYPE_TIMESTAMP:
    case SQL_DATE: case SQL_TIME: case SQL_TIMESTAMP:
      return TextFit::Fraction;
    case SQL_GUID:
      return TextFit::Whole;
    default:
      return info.verboseType == SQL_INTERVAL ? TextFit::Fraction : TextFit::Stream;
  }
}

/// Fits the text of a fixed-format value into `capacity` characters. Only the digits after the
/// first '.' (a number's fraction or the fractional seconds) may be dropped; whatever follows them
/// (an exponent "e+20", a zone) is kept so the value is never changed. False when even the
/// fraction-less form does not fit.
bool fitFixedText(std::string_view text, size_t capacity, bool allowFraction, std::string& out, bool& truncated) {
  truncated = false;
  if (text.size() <= capacity) {
    out.assign(text.data(), text.size());
    return true;
  }
  if (!allowFraction) return false;
  const size_t dot = text.find('.');
  if (dot == std::string_view::npos) return false;
  size_t end = dot + 1;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
  const std::string_view head = text.substr(0, dot);
  const std::string_view tail = text.substr(end);
  const size_t minimal = head.size() + tail.size();
  if (minimal > capacity) return false;
  out.assign(head.data(), head.size());
  const size_t room = capacity - minimal;
  if (room >= 2) out.append(text.data() + dot, room);  // '.' plus room-1 fraction digits
  out.append(tail.data(), tail.size());
  truncated = true;
  return true;
}

/// SQL_C_CHAR / SQL_C_WCHAR for numeric, datetime, interval and GUID columns. These values are never
/// handed out in SQLGetData chunks: every call delivers the whole value (at most with fractional
/// digits dropped, 01004) or fails with 22003 without writing, as Appendix D prescribes.
SQLRETURN deliverFixedText(Ctx& c, std::string_view utf8, bool wide, bool allowFraction, SQLRETURN rcSoFar) {
  std::string narrow;
  std::string_view text = utf8;
  if (!wide && c.opts.ansi == AnsiEncoding::System) {
    narrow = utf8ToNarrow(utf8, c.opts.ansi);
    text = narrow;
  }
  const SQLLEN fullLength = wide ? static_cast<SQLLEN>(wideUnitsFor(text) * sizeof(SQLWCHAR))
                                 : static_cast<SQLLEN>(text.size());
  if (!c.target.buffer) {
    // Length-only request (SQLBindCol with a null data pointer): nothing is consumed.
    c.setIndicator(fullLength);
    return rcSoFar;
  }
  const size_t bufLen = c.target.bufferLength < 0 ? 0 : static_cast<size_t>(c.target.bufferLength);
  const size_t units = wide ? bufLen / sizeof(SQLWCHAR) : bufLen;
  const size_t capacity = units > 0 ? units - 1 : 0;  // room for the NUL terminator

  std::string fitted;
  bool truncated = false;
  // The texts of these types are ASCII, so characters, bytes and SQLWCHAR units coincide.
  if (!fitFixedText(text, capacity, allowFraction, fitted, truncated)) {
    return c.error(sqlstate::kNumericOutOfRange,
                   "Numeric value out of range: a buffer of " + std::to_string(capacity) +
                       " characters cannot hold the whole digits of " + quoted(utf8) + " for " + cTypeName(c.cType));
  }
  if (wide) {
    auto* out = static_cast<SQLWCHAR*>(c.target.buffer);
    size_t consumed = 0;
    const size_t written = fitted.empty() ? 0 : utf8ToWideChunk(fitted, out, capacity, &consumed);
    if (units > 0) out[written] = 0;
  } else {
    auto* out = static_cast<unsigned char*>(c.target.buffer);
    if (!fitted.empty()) std::memcpy(out, fitted.data(), fitted.size());
    if (units > 0) out[fitted.size()] = 0;
  }
  c.setIndicator(fullLength);
  c.markConsumed();
  if (truncated) {
    return mergeReturnCode(rcSoFar, c.warning(sqlstate::kStringTruncated,
                                              "String data, right truncated: fractional digits dropped"));
  }
  return rcSoFar;
}

/// Whole-value delivery into SQL_C_BINARY for non-character/binary SQL types: no chunks, and a
/// buffer shorter than the value is 22003 (Appendix D) rather than a silent cut.
SQLRETURN deliverWholeBinary(Ctx& c, std::string_view bytes) {
  c.setIndicator(static_cast<SQLLEN>(bytes.size()));
  if (!c.target.buffer) return SQL_SUCCESS;
  const size_t bufLen = c.target.bufferLength < 0 ? 0 : static_cast<size_t>(c.target.bufferLength);
  if (bufLen < bytes.size()) {
    return c.error(sqlstate::kNumericOutOfRange, "Numeric value out of range: the " + std::to_string(bytes.size()) +
                                                     "-byte value does not fit a buffer of " +
                                                     std::to_string(bufLen) + " bytes");
  }
  if (!bytes.empty()) std::memcpy(c.target.buffer, bytes.data(), bytes.size());
  c.markConsumed();
  return SQL_SUCCESS;
}

bool parseGuid(std::string_view text, SQLGUID& out);

/// SQL_C_BINARY: String/Binary payloads verbatim; fixed storage kinds as their native representation;
/// a SQL_GUID column as the 16-byte SQLGUID (the same bytes SQL_C_GUID delivers).
SQLRETURN toBinary(Ctx& c) {
  unsigned char native[sizeof(SQLGUID) > 8 ? sizeof(SQLGUID) : 8];
  std::string_view bytes;
  switch (c.data.kind()) {
    case StorageKind::String:
      if (c.col.odbc.conciseType == SQL_GUID) {
        const std::string_view text = trim(rawText(c));
        SQLGUID guid;
        if (!parseGuid(text, guid)) {
          return c.error(sqlstate::kInvalidCharacterValue,
                         "Invalid character value for cast specification: " + quoted(text) + " is not a UUID");
        }
        std::memcpy(native, &guid, sizeof(guid));
        bytes = std::string_view(reinterpret_cast<const char*>(native), sizeof(guid));
        break;
      }
      bytes = c.data.getBytes(c.row);
      break;
    case StorageKind::Binary:
      bytes = c.data.getBytes(c.row);
      break;
    case StorageKind::Bool: {
      native[0] = c.data.getBool(c.row) ? 1 : 0;
      bytes = std::string_view(reinterpret_cast<const char*>(native), 1);
      break;
    }
    case StorageKind::Int64: {
      const int64_t v = c.data.getInt64(c.row);
      size_t n = 8;
      switch (c.col.type.category) {
        case TypeCategory::TinyInt: { const SQLSCHAR t = static_cast<SQLSCHAR>(v); std::memcpy(native, &t, 1); n = 1; break; }
        case TypeCategory::SmallInt: { const SQLSMALLINT t = static_cast<SQLSMALLINT>(v); std::memcpy(native, &t, 2); n = 2; break; }
        case TypeCategory::Integer: { const SQLINTEGER t = static_cast<SQLINTEGER>(v); std::memcpy(native, &t, 4); n = 4; break; }
        default: { const SQLBIGINT t = static_cast<SQLBIGINT>(v); std::memcpy(native, &t, 8); n = 8; break; }
      }
      bytes = std::string_view(reinterpret_cast<const char*>(native), n);
      break;
    }
    case StorageKind::Double: {
      const double v = c.data.getDouble(c.row);
      size_t n = 8;
      if (c.col.type.category == TypeCategory::Real) {
        const SQLREAL f = static_cast<SQLREAL>(v);
        std::memcpy(native, &f, sizeof(f));
        n = sizeof(f);
      } else {
        const SQLDOUBLE d = v;
        std::memcpy(native, &d, sizeof(d));
      }
      bytes = std::string_view(reinterpret_cast<const char*>(native), n);
      break;
    }
  }
  if (textFitFor(c.col.odbc) != TextFit::Stream) return deliverWholeBinary(c, bytes);
  return deliverBytes(c, bytes, false, &binaryCut, SQL_SUCCESS);
}

// ---------------------------------------------------------------------------------------------
// 4. numeric targets
// ---------------------------------------------------------------------------------------------

/// Numeric view of a cell: exact integer, IEEE double, or exact decimal (parsed text).
struct NumValue {
  enum class Kind { Int, Dbl, Dec } kind = Kind::Int;
  int64_t i = 0;
  double d = 0.0;
  Decimal dec;
};

bool parseSpecialDouble(std::string_view s, double* out) {
  if (iequals(s, "nan") || iequals(s, "+nan") || iequals(s, "-nan")) {
    *out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  if (iequals(s, "infinity") || iequals(s, "+infinity") || iequals(s, "inf") || iequals(s, "+inf")) {
    *out = std::numeric_limits<double>::infinity();
    return true;
  }
  if (iequals(s, "-infinity") || iequals(s, "-inf")) {
    *out = -std::numeric_limits<double>::infinity();
    return true;
  }
  return false;
}

/// Reads the cell as a number. Text is trimmed; Decimal/char-like categories only (else 07006).
SQLRETURN readNumber(Ctx& c, NumValue& out) {
  switch (c.data.kind()) {
    case StorageKind::Bool:
      out.kind = NumValue::Kind::Int;
      out.i = c.data.getBool(c.row) ? 1 : 0;
      return SQL_SUCCESS;
    case StorageKind::Int64:
      out.kind = NumValue::Kind::Int;
      out.i = c.data.getInt64(c.row);
      return SQL_SUCCESS;
    case StorageKind::Double:
      out.kind = NumValue::Kind::Dbl;
      out.d = c.data.getDouble(c.row);
      return SQL_SUCCESS;
    case StorageKind::Binary:
      return c.restricted();
    case StorageKind::String:
      break;
  }
  const TypeCategory cat = c.col.type.category;
  if (cat != TypeCategory::Decimal && !isCharLike(cat)) return c.restricted();
  const std::string_view text = trim(rawText(c));
  if (parseDecimal(text, out.dec)) {
    out.kind = NumValue::Kind::Dec;
    return SQL_SUCCESS;
  }
  if (parseSpecialDouble(text, &out.d)) {
    out.kind = NumValue::Kind::Dbl;
    return SQL_SUCCESS;
  }
  return c.error(sqlstate::kInvalidCharacterValue,
                 "Invalid character value for cast specification: " + quoted(text) + " is not a number");
}

std::string numText(const NumValue& v) {
  switch (v.kind) {
    case NumValue::Kind::Int: return std::to_string(v.i);
    case NumValue::Kind::Dbl: return doubleToString(v.d);
    case NumValue::Kind::Dec: return v.dec.toString();
  }
  return {};
}

SQLRETURN outOfRange(Ctx& c, const NumValue& v) {
  return c.error(sqlstate::kNumericOutOfRange,
                 "Numeric value out of range: " + numText(v) + " does not fit " + cTypeName(c.cType));
}

SQLRETURN fractionalTruncation(Ctx& c, const NumValue& v) {
  return c.warning(sqlstate::kFractionalTruncation,
                   "Fractional truncation: " + numText(v) + " truncated for " + cTypeName(c.cType));
}

/// Signed integer target in [lo, hi]. Doubles/decimals truncate toward zero with 01S07.
SQLRETURN toSignedInt(Ctx& c, const NumValue& v, int64_t lo, int64_t hi, int64_t& out) {
  switch (v.kind) {
    case NumValue::Kind::Int:
      if (v.i < lo || v.i > hi) return outOfRange(c, v);
      out = v.i;
      return SQL_SUCCESS;
    case NumValue::Kind::Dbl: {
      if (std::isnan(v.d) || std::isinf(v.d)) return outOfRange(c, v);
      const double t = std::trunc(v.d);
      // (double)INT64_MAX rounds up to 2^63, so compare against the exclusive bound.
      if (t < -9223372036854775808.0 || t >= 9223372036854775808.0) return outOfRange(c, v);
      const int64_t iv = static_cast<int64_t>(t);
      if (iv < lo || iv > hi) return outOfRange(c, v);
      out = iv;
      return t != v.d ? fractionalTruncation(c, v) : SQL_SUCCESS;
    }
    case NumValue::Kind::Dec: {
      bool lost = false;
      int64_t iv = 0;
      if (!decimalToInt64(v.dec, iv, &lost)) return outOfRange(c, v);
      if (iv < lo || iv > hi) return outOfRange(c, v);
      out = iv;
      return lost ? fractionalTruncation(c, v) : SQL_SUCCESS;
    }
  }
  return outOfRange(c, v);
}

/// Unsigned integer target in [0, hi].
SQLRETURN toUnsignedInt(Ctx& c, const NumValue& v, uint64_t hi, uint64_t& out) {
  switch (v.kind) {
    case NumValue::Kind::Int:
      if (v.i < 0 || static_cast<uint64_t>(v.i) > hi) return outOfRange(c, v);
      out = static_cast<uint64_t>(v.i);
      return SQL_SUCCESS;
    case NumValue::Kind::Dbl: {
      if (std::isnan(v.d) || std::isinf(v.d)) return outOfRange(c, v);
      const double t = std::trunc(v.d);
      if (t < 0.0 || t >= 18446744073709551616.0) return outOfRange(c, v);
      const uint64_t uv = static_cast<uint64_t>(t);
      if (uv > hi) return outOfRange(c, v);
      out = uv;
      return t != v.d ? fractionalTruncation(c, v) : SQL_SUCCESS;
    }
    case NumValue::Kind::Dec: {
      bool lost = false;
      if (v.dec.negative) {
        // Only values that truncate to zero (e.g. -0.5) are representable.
        int64_t iv = 0;
        if (!decimalToInt64(v.dec, iv, &lost) || iv != 0) return outOfRange(c, v);
        out = 0;
        return lost ? fractionalTruncation(c, v) : SQL_SUCCESS;
      }
      uint64_t uv = 0;
      if (!decimalToUInt64(v.dec, uv, &lost)) return outOfRange(c, v);
      if (uv > hi) return outOfRange(c, v);
      out = uv;
      return lost ? fractionalTruncation(c, v) : SQL_SUCCESS;
    }
  }
  return outOfRange(c, v);
}

template <typename T>
SQLRETURN emitSigned(Ctx& c, const NumValue& v) {
  int64_t iv = 0;
  const SQLRETURN rc = toSignedInt(c, v, static_cast<int64_t>(std::numeric_limits<T>::min()),
                                   static_cast<int64_t>(std::numeric_limits<T>::max()), iv);
  return emitFixed(c, static_cast<T>(iv), rc);
}

template <typename T>
SQLRETURN emitUnsigned(Ctx& c, const NumValue& v) {
  uint64_t uv = 0;
  const SQLRETURN rc = toUnsignedInt(c, v, static_cast<uint64_t>(std::numeric_limits<T>::max()), uv);
  return emitFixed(c, static_cast<T>(uv), rc);
}

SQLRETURN toDouble(Ctx& c, const NumValue& v, double& out) {
  switch (v.kind) {
    case NumValue::Kind::Int:
      out = static_cast<double>(v.i);
      return SQL_SUCCESS;
    case NumValue::Kind::Dbl:
      out = v.d;
      return SQL_SUCCESS;
    case NumValue::Kind::Dec:
      out = decimalToDouble(v.dec);
      if (std::isinf(out)) return outOfRange(c, v);
      return SQL_SUCCESS;
  }
  return outOfRange(c, v);
}

SQLRETURN emitDouble(Ctx& c, const NumValue& v) {
  double d = 0.0;
  const SQLRETURN rc = toDouble(c, v, d);
  return emitFixed(c, static_cast<SQLDOUBLE>(d), rc);
}

SQLRETURN emitFloat(Ctx& c, const NumValue& v) {
  double d = 0.0;
  SQLRETURN rc = toDouble(c, v, d);
  if (rc != SQL_ERROR && std::isfinite(d) && std::fabs(d) > static_cast<double>(FLT_MAX)) rc = outOfRange(c, v);
  return emitFixed(c, static_cast<SQLREAL>(d), rc);
}

/// SQL_C_BIT: 0/1 exact; (0,2) with a fraction -> 01S07 truncated; anything else 22003.
SQLRETURN emitBit(Ctx& c, const NumValue& v) {
  unsigned char bit = 0;
  SQLRETURN rc = SQL_SUCCESS;
  switch (v.kind) {
    case NumValue::Kind::Int:
      if (v.i != 0 && v.i != 1) return outOfRange(c, v);
      bit = static_cast<unsigned char>(v.i);
      break;
    case NumValue::Kind::Dbl: {
      if (std::isnan(v.d) || v.d < 0.0 || v.d >= 2.0) return outOfRange(c, v);
      const double t = std::trunc(v.d);
      bit = static_cast<unsigned char>(t);
      if (t != v.d) rc = fractionalTruncation(c, v);
      break;
    }
    case NumValue::Kind::Dec: {
      bool lost = false;
      int64_t iv = 0;
      if ((v.dec.negative && !v.dec.isZero()) || !decimalToInt64(v.dec, iv, &lost) || iv < 0 || iv > 1) {
        return outOfRange(c, v);
      }
      bit = static_cast<unsigned char>(iv);
      if (lost) rc = fractionalTruncation(c, v);
      break;
    }
  }
  return emitFixed(c, bit, rc);
}

/// SQL_C_NUMERIC with precision/scale from the ARD when set, else the column (exact numerics) or
/// the driver default (38, 0). Lost fraction digits -> 01S07; precision exceeded -> 22003.
SQLRETURN emitNumeric(Ctx& c, const NumValue& v) {
  Decimal d;
  switch (v.kind) {
    case NumValue::Kind::Int:
      d = decimalFromInt64(v.i);
      break;
    case NumValue::Kind::Dbl:
      if (std::isnan(v.d) || std::isinf(v.d)) return outOfRange(c, v);
      d = decimalFromDouble(v.d);
      break;
    case NumValue::Kind::Dec:
      d = v.dec;
      break;
  }
  SQLSMALLINT precision = c.target.numericPrecision;
  if (precision <= 0) {
    precision = isExactNumericCategory(c.col.type.category) && c.col.odbc.precision > 0 ? c.col.odbc.precision
                                                                                         : kDefaultNumericPrecision;
  }
  if (precision > kDefaultNumericPrecision) precision = kDefaultNumericPrecision;
  SQLSMALLINT scale = 0;
  if (c.target.numericScaleSet) {
    scale = c.target.numericScale;
  } else if (c.col.type.category == TypeCategory::Decimal) {
    scale = c.col.odbc.decimalDigits;
  }
  if (scale < 0) scale = 0;
  if (scale > precision) scale = precision;

  // Truncate (never round) so the 01S07 semantics are the ODBC ones regardless of numeric.cpp's policy.
  const bool lost = rescaleDecimal(d, scale, false);
  SQL_NUMERIC_STRUCT out;
  std::memset(&out, 0, sizeof(out));
  bool lostMore = false;
  if (!decimalToNumeric(d, precision, scale, out, &lostMore)) {
    return c.error(sqlstate::kNumericOutOfRange, "Numeric value out of range: " + numText(v) +
                                                     " exceeds precision " + std::to_string(precision) +
                                                     " scale " + std::to_string(scale));
  }
  const SQLRETURN rc = (lost || lostMore) ? fractionalTruncation(c, v) : SQL_SUCCESS;
  return emitFixed(c, out, rc);
}

SQLRETURN toNumericTarget(Ctx& c) {
  NumValue v;
  if (c.data.kind() == StorageKind::String && c.cType == SQL_C_BIT && isCharLike(c.col.type.category)) {
    // Character "true"/"false" are accepted by SQL_C_BIT before the numeric parse.
    const std::string_view text = trim(rawText(c));
    if (iequals(text, "true") || iequals(text, "false")) {
      v.kind = NumValue::Kind::Int;
      v.i = iequals(text, "true") ? 1 : 0;
      return emitBit(c, v);
    }
  }
  const SQLRETURN rc = readNumber(c, v);
  if (rc == SQL_ERROR) return rc;
  switch (c.cType) {
    case SQL_C_STINYINT:
    case SQL_C_TINYINT: return emitSigned<SQLSCHAR>(c, v);
    case SQL_C_UTINYINT: return emitUnsigned<SQLCHAR>(c, v);
    case SQL_C_SSHORT:
    case SQL_C_SHORT: return emitSigned<SQLSMALLINT>(c, v);
    case SQL_C_USHORT: return emitUnsigned<SQLUSMALLINT>(c, v);
    case SQL_C_SLONG:
    case SQL_C_LONG: return emitSigned<SQLINTEGER>(c, v);
    case SQL_C_ULONG: return emitUnsigned<SQLUINTEGER>(c, v);
    case SQL_C_SBIGINT: return emitSigned<SQLBIGINT>(c, v);
    case SQL_C_UBIGINT: return emitUnsigned<SQLUBIGINT>(c, v);
    case SQL_C_FLOAT: return emitFloat(c, v);
    case SQL_C_DOUBLE: return emitDouble(c, v);
    case SQL_C_BIT: return emitBit(c, v);
    case SQL_C_NUMERIC: return emitNumeric(c, v);
    default: return c.restricted();
  }
}

// ---------------------------------------------------------------------------------------------
// 5. temporal targets
// ---------------------------------------------------------------------------------------------

struct TemporalValue {
  bool hasDate = false;
  bool hasTime = false;
  CivilDate date;
  CivilTime time;
  bool subNanoLost = false;  // text carried non-zero digits beyond nanoseconds
};

/// Applies the zone handling of a timestamp with time zone according to the mode.
SQLRETURN applyTimestampZone(Ctx& c, ParsedTimestamp& ts) {
  return applyTimestampTzMode(ts, c.opts, &c.diag, c.rowNumber, c.columnNumber);
}

/// Applies the offset handling of a time with time zone according to the mode.
SQLRETURN applyTimeZone(Ctx& c, CivilTime& t, bool hasOffset, int32_t offset) {
  return applyTimeTzMode(t, hasOffset, offset, c.opts, &c.diag, c.rowNumber, c.columnNumber);
}

SQLRETURN invalidTemporalText(Ctx& c, std::string_view text) {
  return c.error(sqlstate::kInvalidCharacterValue,
                 "Invalid character value for cast specification: " + quoted(text) + " is not a valid " +
                     std::string(cTypeName(c.cType)) + " value");
}

/// Reads the cell as a civil date/time applying the configured time-zone modes.
SQLRETURN readTemporal(Ctx& c, TemporalValue& out) {
  if (c.data.kind() != StorageKind::String) return c.restricted();
  const std::string_view raw = rawText(c);
  const TypeCategory cat = c.col.type.category;
  switch (cat) {
    case TypeCategory::Date:
      if (!parseTrinoDate(raw, out.date)) return invalidTemporalText(c, raw);
      out.hasDate = true;
      return SQL_SUCCESS;
    case TypeCategory::Time:
    case TypeCategory::TimeWithTimeZone: {
      int32_t offset = 0;
      bool hasOffset = false;
      if (!parseTrinoTime(raw, out.time, &offset, &hasOffset)) return invalidTemporalText(c, raw);
      const SQLRETURN rc = cat == TypeCategory::TimeWithTimeZone ? applyTimeZone(c, out.time, hasOffset, offset)
                                                                 : SQL_SUCCESS;
      out.hasTime = true;
      out.subNanoLost = hasNonZeroSubNanoDigits(raw);
      return rc;
    }
    case TypeCategory::Timestamp:
    case TypeCategory::TimestampWithTimeZone: {
      ParsedTimestamp ts;
      if (!parseTrinoTimestamp(raw, ts)) return invalidTemporalText(c, raw);
      const SQLRETURN rc = cat == TypeCategory::TimestampWithTimeZone ? applyTimestampZone(c, ts) : SQL_SUCCESS;
      out.date = ts.date;
      out.time = ts.time;
      out.hasDate = out.hasTime = true;
      out.subNanoLost = hasNonZeroSubNanoDigits(raw);
      return rc;
    }
    default:
      break;
  }
  if (!isCharLike(cat)) return c.restricted();
  // Character data: accept timestamp, date or time literal forms (ODBC char -> datetime rules).
  const std::string_view text = trim(raw);
  ParsedTimestamp ts;
  if (parseTrinoTimestamp(text, ts)) {
    const SQLRETURN rc = applyTimestampZone(c, ts);
    out.date = ts.date;
    out.time = ts.time;
    out.hasDate = out.hasTime = true;
    out.subNanoLost = hasNonZeroSubNanoDigits(text);
    return rc;
  }
  if (parseTrinoDate(text, out.date)) {
    out.hasDate = true;
    return SQL_SUCCESS;
  }
  int32_t offset = 0;
  bool hasOffset = false;
  if (parseTrinoTime(text, out.time, &offset, &hasOffset)) {
    const SQLRETURN rc = applyTimeZone(c, out.time, hasOffset, offset);
    out.hasTime = true;
    out.subNanoLost = hasNonZeroSubNanoDigits(text);
    return rc;
  }
  return invalidTemporalText(c, text);
}

bool yearFitsStruct(int32_t year) {
  return year >= std::numeric_limits<SQLSMALLINT>::min() && year <= std::numeric_limits<SQLSMALLINT>::max();
}

SQLRETURN datetimeOverflow(Ctx& c, const TemporalValue& v) {
  return c.error(sqlstate::kDatetimeOverflow,
                 "Datetime field overflow: year " + std::to_string(v.date.year) + " does not fit " + cTypeName(c.cType));
}

SQLRETURN toTemporalTarget(Ctx& c) {
  TemporalValue v;
  SQLRETURN rc = readTemporal(c, v);
  if (rc == SQL_ERROR) return rc;
  const bool timeNonZero = v.time.hour != 0 || v.time.minute != 0 || v.time.second != 0 || v.time.nanos != 0;

  switch (c.cType) {
    case SQL_C_TYPE_DATE: {
      if (!v.hasDate) return c.restricted();
      if (!yearFitsStruct(v.date.year)) return datetimeOverflow(c, v);
      SQL_DATE_STRUCT out;
      std::memset(&out, 0, sizeof(out));
      out.year = static_cast<SQLSMALLINT>(v.date.year);
      out.month = static_cast<SQLUSMALLINT>(v.date.month);
      out.day = static_cast<SQLUSMALLINT>(v.date.day);
      if (v.hasTime && (timeNonZero || v.subNanoLost)) {
        rc = mergeReturnCode(rc, c.warning(sqlstate::kFractionalTruncation,
                                           "Fractional truncation: time portion dropped converting to SQL_C_TYPE_DATE"));
      }
      return emitFixed(c, out, rc);
    }
    case SQL_C_TYPE_TIME: {
      if (!v.hasTime) return c.restricted();
      SQL_TIME_STRUCT out;
      std::memset(&out, 0, sizeof(out));
      out.hour = static_cast<SQLUSMALLINT>(v.time.hour);
      out.minute = static_cast<SQLUSMALLINT>(v.time.minute);
      out.second = static_cast<SQLUSMALLINT>(v.time.second);
      // The date part of a timestamp is dropped silently (spec); fractional seconds are not.
      if (v.time.nanos != 0 || v.subNanoLost) {
        rc = mergeReturnCode(rc, c.warning(sqlstate::kFractionalTruncation,
                                           "Fractional truncation: fractional seconds dropped converting to SQL_C_TYPE_TIME"));
      }
      return emitFixed(c, out, rc);
    }
    case SQL_C_TYPE_TIMESTAMP: {
      CivilDate date = v.date;
      if (!v.hasDate) date = currentCivilDate(c.opts.sessionTimeZone);  // time -> timestamp takes the current date (spec)
      if (!yearFitsStruct(date.year)) {
        v.date = date;
        return datetimeOverflow(c, v);
      }
      SQL_TIMESTAMP_STRUCT out;
      std::memset(&out, 0, sizeof(out));
      out.year = static_cast<SQLSMALLINT>(date.year);
      out.month = static_cast<SQLUSMALLINT>(date.month);
      out.day = static_cast<SQLUSMALLINT>(date.day);
      if (v.hasTime) {
        out.hour = static_cast<SQLUSMALLINT>(v.time.hour);
        out.minute = static_cast<SQLUSMALLINT>(v.time.minute);
        out.second = static_cast<SQLUSMALLINT>(v.time.second);
        out.fraction = static_cast<SQLUINTEGER>(v.time.nanos);
      }
      if (v.subNanoLost) {
        rc = mergeReturnCode(rc, c.warning(sqlstate::kFractionalTruncation,
                                           "Fractional truncation: sub-nanosecond digits dropped converting to SQL_C_TYPE_TIMESTAMP"));
      }
      return emitFixed(c, out, rc);
    }
    default:
      return c.restricted();
  }
}

// ---------------------------------------------------------------------------------------------
// 6. interval and GUID targets
// ---------------------------------------------------------------------------------------------

struct CanonicalInterval {
  bool yearMonth = false;
  bool negative = false;
  uint64_t years = 0, months = 0;
  uint64_t days = 0, hours = 0, minutes = 0, seconds = 0, nanos = 0;
};

SQLINTERVAL intervalCodeFor(SQLSMALLINT cType) {
  switch (cType) {
    case SQL_C_INTERVAL_YEAR: return SQL_IS_YEAR;
    case SQL_C_INTERVAL_MONTH: return SQL_IS_MONTH;
    case SQL_C_INTERVAL_DAY: return SQL_IS_DAY;
    case SQL_C_INTERVAL_HOUR: return SQL_IS_HOUR;
    case SQL_C_INTERVAL_MINUTE: return SQL_IS_MINUTE;
    case SQL_C_INTERVAL_SECOND: return SQL_IS_SECOND;
    case SQL_C_INTERVAL_YEAR_TO_MONTH: return SQL_IS_YEAR_TO_MONTH;
    case SQL_C_INTERVAL_DAY_TO_HOUR: return SQL_IS_DAY_TO_HOUR;
    case SQL_C_INTERVAL_DAY_TO_MINUTE: return SQL_IS_DAY_TO_MINUTE;
    case SQL_C_INTERVAL_DAY_TO_SECOND: return SQL_IS_DAY_TO_SECOND;
    case SQL_C_INTERVAL_HOUR_TO_MINUTE: return SQL_IS_HOUR_TO_MINUTE;
    case SQL_C_INTERVAL_HOUR_TO_SECOND: return SQL_IS_HOUR_TO_SECOND;
    default: return SQL_IS_MINUTE_TO_SECOND;
  }
}

SQLRETURN intervalOverflow(Ctx& c) {
  return c.error(sqlstate::kIntervalOverflow,
                 std::string("Interval field overflow: leading field does not fit ") + cTypeName(c.cType));
}

/// Interval seconds precision of the C buffer: the ARD SQL_DESC_PRECISION when set, else the ODBC
/// default of 6. The column's own fractional digits describe the SQL type (IRD), not the buffer, so
/// they must not be used here: an application reading the default layout expects microseconds.
int intervalSecondsPrecision(const Ctx& c) {
  int p = c.target.numericPrecision;
  if (p <= 0) p = kDefaultIntervalSecondsPrecision;
  if (p > 9) p = 9;
  return p;
}

/// Regroups the canonical fields into the layout of the requested SQL_C_INTERVAL_* type.
SQLRETURN projectInterval(Ctx& c, const CanonicalInterval& v, SQL_INTERVAL_STRUCT& out) {
  std::memset(&out, 0, sizeof(out));
  out.interval_type = intervalCodeFor(c.cType);
  out.interval_sign = v.negative ? SQL_TRUE : SQL_FALSE;
  const uint64_t maxField = std::numeric_limits<SQLUINTEGER>::max();
  bool trailingLost = false;

  if (v.yearMonth) {
    if (!isYearMonthCType(c.cType)) return c.restricted();
    SQL_YEAR_MONTH_STRUCT& ym = out.intval.year_month;
    switch (c.cType) {
      case SQL_C_INTERVAL_YEAR:
        if (v.years > maxField) return intervalOverflow(c);
        ym.year = static_cast<SQLUINTEGER>(v.years);
        trailingLost = v.months != 0;
        break;
      case SQL_C_INTERVAL_MONTH: {
        const uint64_t total = v.years * 12 + v.months;
        if (v.years > maxField / 12 || total > maxField) return intervalOverflow(c);
        ym.month = static_cast<SQLUINTEGER>(total);
        break;
      }
      default:
        if (v.years > maxField || v.months > maxField) return intervalOverflow(c);
        ym.year = static_cast<SQLUINTEGER>(v.years);
        ym.month = static_cast<SQLUINTEGER>(v.months);
        break;
    }
  } else {
    if (!isDaySecondCType(c.cType)) return c.restricted();
    SQL_DAY_SECOND_STRUCT& ds = out.intval.day_second;
    // Leading-field totals; the guards keep the multiplications inside uint64.
    if (v.days > maxField || v.hours > maxField || v.minutes > maxField || v.seconds > maxField) {
      return intervalOverflow(c);
    }
    const uint64_t totalHours = v.days * 24 + v.hours;
    const uint64_t totalMinutes = totalHours * 60 + v.minutes;
    const uint64_t totalSeconds = totalMinutes * 60 + v.seconds;
    const int precision = intervalSecondsPrecision(c);
    const int64_t divisor = pow10i(9 - precision);
    const uint64_t fraction = v.nanos / static_cast<uint64_t>(divisor);
    const bool fractionLost = v.nanos % static_cast<uint64_t>(divisor) != 0;
    switch (c.cType) {
      case SQL_C_INTERVAL_DAY:
        ds.day = static_cast<SQLUINTEGER>(v.days);
        trailingLost = v.hours || v.minutes || v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_HOUR:
        if (totalHours > maxField) return intervalOverflow(c);
        ds.hour = static_cast<SQLUINTEGER>(totalHours);
        trailingLost = v.minutes || v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_MINUTE:
        if (totalMinutes > maxField) return intervalOverflow(c);
        ds.minute = static_cast<SQLUINTEGER>(totalMinutes);
        trailingLost = v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_SECOND:
        if (totalSeconds > maxField) return intervalOverflow(c);
        ds.second = static_cast<SQLUINTEGER>(totalSeconds);
        ds.fraction = static_cast<SQLUINTEGER>(fraction);
        trailingLost = fractionLost;
        break;
      case SQL_C_INTERVAL_DAY_TO_HOUR:
        ds.day = static_cast<SQLUINTEGER>(v.days);
        ds.hour = static_cast<SQLUINTEGER>(v.hours);
        trailingLost = v.minutes || v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_DAY_TO_MINUTE:
        ds.day = static_cast<SQLUINTEGER>(v.days);
        ds.hour = static_cast<SQLUINTEGER>(v.hours);
        ds.minute = static_cast<SQLUINTEGER>(v.minutes);
        trailingLost = v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_DAY_TO_SECOND:
        ds.day = static_cast<SQLUINTEGER>(v.days);
        ds.hour = static_cast<SQLUINTEGER>(v.hours);
        ds.minute = static_cast<SQLUINTEGER>(v.minutes);
        ds.second = static_cast<SQLUINTEGER>(v.seconds);
        ds.fraction = static_cast<SQLUINTEGER>(fraction);
        trailingLost = fractionLost;
        break;
      case SQL_C_INTERVAL_HOUR_TO_MINUTE:
        if (totalHours > maxField) return intervalOverflow(c);
        ds.hour = static_cast<SQLUINTEGER>(totalHours);
        ds.minute = static_cast<SQLUINTEGER>(v.minutes);
        trailingLost = v.seconds || v.nanos;
        break;
      case SQL_C_INTERVAL_HOUR_TO_SECOND:
        if (totalHours > maxField) return intervalOverflow(c);
        ds.hour = static_cast<SQLUINTEGER>(totalHours);
        ds.minute = static_cast<SQLUINTEGER>(v.minutes);
        ds.second = static_cast<SQLUINTEGER>(v.seconds);
        ds.fraction = static_cast<SQLUINTEGER>(fraction);
        trailingLost = fractionLost;
        break;
      default:  // SQL_C_INTERVAL_MINUTE_TO_SECOND
        if (totalMinutes > maxField) return intervalOverflow(c);
        ds.minute = static_cast<SQLUINTEGER>(totalMinutes);
        ds.second = static_cast<SQLUINTEGER>(v.seconds);
        ds.fraction = static_cast<SQLUINTEGER>(fraction);
        trailingLost = fractionLost;
        break;
    }
  }
  if (trailingLost) {
    return c.warning(sqlstate::kFractionalTruncation,
                     std::string("Fractional truncation: trailing interval fields dropped converting to ") +
                         cTypeName(c.cType));
  }
  return SQL_SUCCESS;
}

void canonicalFromStruct(const SQL_INTERVAL_STRUCT& s, bool yearMonth, CanonicalInterval& out) {
  out = CanonicalInterval{};
  out.yearMonth = yearMonth;
  out.negative = s.interval_sign == SQL_TRUE;
  if (yearMonth) {
    out.years = s.intval.year_month.year;
    out.months = s.intval.year_month.month;
  } else {
    out.days = s.intval.day_second.day;
    out.hours = s.intval.day_second.hour;
    out.minutes = s.intval.day_second.minute;
    out.seconds = s.intval.day_second.second;
    out.nanos = s.intval.day_second.fraction;  // temporal.h: nanoseconds
  }
}

SQLRETURN invalidIntervalText(Ctx& c, std::string_view text) {
  return c.error(sqlstate::kInvalidCharacterValue,
                 "Invalid character value for cast specification: " + quoted(text) + " is not a valid interval for " +
                     cTypeName(c.cType));
}

/// Exact numeric -> single-field interval (ODBC: only single-field interval targets are allowed).
SQLRETURN intervalFromNumber(Ctx& c, CanonicalInterval& out) {
  NumValue v;
  const SQLRETURN rc = readNumber(c, v);
  if (rc == SQL_ERROR) return rc;
  if (v.kind == NumValue::Kind::Dbl) return c.restricted();  // approximate numerics are not convertible
  switch (c.cType) {
    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND:
      break;
    default:
      return c.restricted();
  }
  out = CanonicalInterval{};
  out.yearMonth = isYearMonthCType(c.cType);
  uint64_t magnitude = 0;
  uint64_t nanos = 0;
  bool lost = false;
  if (v.kind == NumValue::Kind::Int) {
    out.negative = v.i < 0;
    magnitude = v.i < 0 ? (static_cast<uint64_t>(-(v.i + 1)) + 1) : static_cast<uint64_t>(v.i);
  } else {
    Decimal whole = v.dec;
    out.negative = whole.negative && !whole.isZero();
    whole.negative = false;
    if (c.cType == SQL_C_INTERVAL_SECOND) {
      // Keep up to nine fractional digits as nanoseconds; further digits are lost with 01S07.
      Decimal scaled = whole;
      lost = rescaleDecimal(scaled, 9, false);
      scaled.scale = 0;  // digits now count nanoseconds
      uint64_t units = 0;
      if (!decimalToUInt64(scaled, units, nullptr)) return intervalOverflow(c);
      magnitude = units / static_cast<uint64_t>(kNanosPerSecond);
      nanos = units % static_cast<uint64_t>(kNanosPerSecond);
    } else {
      if (!decimalToUInt64(whole, magnitude, &lost)) return intervalOverflow(c);
    }
  }
  if (magnitude > std::numeric_limits<SQLUINTEGER>::max()) return intervalOverflow(c);
  switch (c.cType) {
    case SQL_C_INTERVAL_YEAR: out.years = magnitude; break;
    case SQL_C_INTERVAL_MONTH: out.months = magnitude; break;
    case SQL_C_INTERVAL_DAY: out.days = magnitude; break;
    case SQL_C_INTERVAL_HOUR: out.hours = magnitude; break;
    case SQL_C_INTERVAL_MINUTE: out.minutes = magnitude; break;
    default: out.seconds = magnitude; out.nanos = nanos; break;
  }
  return lost ? fractionalTruncation(c, v) : SQL_SUCCESS;
}

SQLRETURN toIntervalTarget(Ctx& c) {
  CanonicalInterval v;
  SQLRETURN rc = SQL_SUCCESS;
  const TypeCategory cat = c.col.type.category;
  if (c.data.kind() == StorageKind::String &&
      (cat == TypeCategory::IntervalYearToMonth || cat == TypeCategory::IntervalDayToSecond || isCharLike(cat))) {
    const std::string_view text = trim(rawText(c));
    SQL_INTERVAL_STRUCT parsed;
    bool yearMonth = false;
    if (cat == TypeCategory::IntervalYearToMonth) {
      yearMonth = true;
    } else if (cat == TypeCategory::IntervalDayToSecond) {
      yearMonth = false;
    } else if (isYearMonthCType(c.cType)) {
      yearMonth = true;
    } else if (isDaySecondCType(c.cType)) {
      yearMonth = false;
    } else {
      return c.restricted();
    }
    const bool ok = yearMonth ? parseTrinoIntervalYearMonth(text, parsed) : parseTrinoIntervalDaySecond(text, parsed);
    if (!ok) return invalidIntervalText(c, text);
    canonicalFromStruct(parsed, yearMonth, v);
  } else if (c.data.kind() == StorageKind::Int64 || (c.data.kind() == StorageKind::String && cat == TypeCategory::Decimal)) {
    rc = intervalFromNumber(c, v);
    if (rc == SQL_ERROR) return rc;
  } else {
    return c.restricted();
  }
  SQL_INTERVAL_STRUCT out;
  const SQLRETURN prc = projectInterval(c, v, out);
  if (prc == SQL_ERROR) return prc;
  return emitFixed(c, out, mergeReturnCode(rc, prc));
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (optionally in braces) -> SQLGUID.
bool parseGuid(std::string_view text, SQLGUID& out) {
  if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
  if (text.size() != 36) return false;
  unsigned char bytes[16];
  size_t bi = 0;
  for (size_t i = 0; i < 36;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
      ++i;
      continue;
    }
    const int hi = hexValue(text[i]);
    const int lo = hexValue(text[i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[bi++] = static_cast<unsigned char>(hi * 16 + lo);
    i += 2;
  }
  std::memset(&out, 0, sizeof(out));
  out.Data1 = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
              (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
  out.Data2 = static_cast<unsigned short>((bytes[4] << 8) | bytes[5]);
  out.Data3 = static_cast<unsigned short>((bytes[6] << 8) | bytes[7]);
  std::memcpy(out.Data4, bytes + 8, 8);
  return true;
}

SQLRETURN toGuidTarget(Ctx& c) {
  if (c.data.kind() != StorageKind::String) return c.restricted();
  const TypeCategory cat = c.col.type.category;
  if (cat != TypeCategory::Uuid && !isCharLike(cat)) return c.restricted();
  const std::string_view text = trim(rawText(c));
  SQLGUID guid;
  if (!parseGuid(text, guid)) {
    return c.error(sqlstate::kInvalidCharacterValue,
                   "Invalid character value for cast specification: " + quoted(text) + " is not a UUID");
  }
  return emitFixed(c, guid, SQL_SUCCESS);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// 7. public entry points
// ---------------------------------------------------------------------------------------------

std::string cellToText(const ResultColumn& col, const Column& data, size_t row, const ConvertOptions& opts) {
  if (data.isNull(row)) return {};
  std::string scratch;
  std::string_view view;
  renderText(col, data, row, opts, nullptr, SQL_NO_ROW_NUMBER, SQL_NO_COLUMN_NUMBER, scratch, view);
  return std::string(view);
}

SQLRETURN convertToC(const ResultColumn& col, const Column& data, size_t row, const ConvertTarget& target,
                     const ConvertOptions& opts, size_t* chunkOffset, Diag& diag, SQLLEN rowNumber,
                     SQLINTEGER columnNumber) {
  SQLSMALLINT cType = target.cType;
  if (cType == SQL_C_DEFAULT) {
    cType = col.odbc.defaultCType != 0 ? col.odbc.defaultCType : defaultCTypeFor(col.odbc.conciseType);
  }
  cType = normalizeLegacyCType(cType);
  Ctx c{col, data, row, target, opts, chunkOffset, diag, rowNumber, columnNumber, cType};

  if (chunkOffset && *chunkOffset == SIZE_MAX) return SQL_NO_DATA;  // everything already delivered

  if (row >= data.size()) {
    return c.error(sqlstate::kGeneralError, "Internal error: row index out of range for column data");
  }

  if (data.isNull(row)) {
    if (!target.strLenOrInd) {
      return c.error(sqlstate::kIndicatorRequired, "Indicator variable required but not supplied");
    }
    *target.strLenOrInd = SQL_NULL_DATA;
    c.markConsumed();
    return SQL_SUCCESS;
  }

  switch (cType) {
    case SQL_C_CHAR:
    case SQL_C_WCHAR: {
      std::string scratch;
      std::string_view text;
      const SQLRETURN rc = renderText(col, data, row, opts, &diag, rowNumber, columnNumber, scratch, text);
      if (rc == SQL_ERROR) return rc;
      const TextFit fit = textFitFor(col.odbc);
      if (fit != TextFit::Stream) return deliverFixedText(c, text, cType == SQL_C_WCHAR, fit == TextFit::Fraction, rc);
      return cType == SQL_C_CHAR ? toChar(c, text, rc) : toWChar(c, text, rc);
    }
    case SQL_C_BINARY:
      return toBinary(c);
    case SQL_C_STINYINT:
    case SQL_C_TINYINT:
    case SQL_C_UTINYINT:
    case SQL_C_SSHORT:
    case SQL_C_SHORT:
    case SQL_C_USHORT:
    case SQL_C_SLONG:
    case SQL_C_LONG:
    case SQL_C_ULONG:
    case SQL_C_SBIGINT:
    case SQL_C_UBIGINT:
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE:
    case SQL_C_BIT:
    case SQL_C_NUMERIC:
      return toNumericTarget(c);
    case SQL_C_TYPE_DATE:
    case SQL_C_TYPE_TIME:
    case SQL_C_TYPE_TIMESTAMP:
      return toTemporalTarget(c);
    case SQL_C_GUID:
      return toGuidTarget(c);
    default:
      if (isIntervalCType(cType)) return toIntervalTarget(c);
      return c.error(sqlstate::kInvalidAppBufferType,
                     "Invalid application buffer type " + std::to_string(static_cast<int>(target.cType)));
  }
}

}  // namespace trino_odbc
