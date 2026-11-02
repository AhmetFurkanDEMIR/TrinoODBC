// Exact decimal arithmetic and shortest round-trip floating-point text.
//
// Design notes:
//  * Decimal magnitudes are ASCII digit strings; the only arithmetic needed is rounding (increment)
//    and base-10^9 schoolbook conversion to/from the 128-bit SQL_NUMERIC_STRUCT magnitude. That keeps
//    the code obviously correct for the full 38-digit range without a big-integer dependency.
//  * Floating point <-> text never goes through the C locale: the platform's shortest algorithm
//    (std::to_chars when the library has it) or a printf probe loop produces the shortest digits, and
//    the final layout is produced by our own formatter so every platform prints the same text.
#include "convert/numeric.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#  define TRINO_ODBC_HAVE_FP_TO_CHARS 1
#  include <charconv>
#elif defined(TRINO_ODBC_UNIX)
#  include <clocale>
#  if defined(__APPLE__)
#    include <xlocale.h>
#  endif
#endif

namespace trino_odbc {

namespace {

constexpr uint32_t kBase = 1000000000u;  // 10^9: the largest power of ten that fits a 32-bit limb

/// 128-bit unsigned magnitude as four little-endian 32-bit limbs (the SQL_NUMERIC_STRUCT layout).
struct U128 {
  uint32_t w[4] = {0, 0, 0, 0};
  bool isZero() const { return (w[0] | w[1] | w[2] | w[3]) == 0; }
};

/// x = x * mul + add. Returns true when the result no longer fits in 128 bits.
bool mulAdd(U128& x, uint32_t mul, uint32_t add) {
  uint64_t carry = add;
  for (uint32_t& limb : x.w) {
    const uint64_t t = static_cast<uint64_t>(limb) * mul + carry;
    limb = static_cast<uint32_t>(t);
    carry = t >> 32;
  }
  return carry != 0;
}

/// x /= div; returns the remainder.
uint32_t divMod(U128& x, uint32_t div) {
  uint64_t rem = 0;
  for (int i = 3; i >= 0; --i) {
    const uint64_t cur = (rem << 32) | x.w[i];
    x.w[i] = static_cast<uint32_t>(cur / div);
    rem = cur % div;
  }
  return static_cast<uint32_t>(rem);
}

bool allZeroDigits(std::string_view s) {
  for (char c : s) {
    if (c != '0') return false;
  }
  return true;
}

/// Removes leading zeros, leaving at least one digit.
void stripLeadingZeros(std::string& digits) {
  size_t i = 0;
  while (i + 1 < digits.size() && digits[i] == '0') ++i;
  if (i) digits.erase(0, i);
  if (digits.empty()) digits = "0";
}

/// Adds one to a digit-string magnitude (used for round-half-away-from-zero).
void incrementDigits(std::string& digits) {
  for (size_t i = digits.size(); i-- > 0;) {
    if (digits[i] != '9') {
      ++digits[i];
      return;
    }
    digits[i] = '0';
  }
  digits.insert(digits.begin(), '1');
}

void canonicalize(Decimal& d) {
  stripLeadingZeros(d.digits);
  if (d.scale < 0) {
    d.digits.append(static_cast<size_t>(-d.scale), '0');
    d.scale = 0;
    stripLeadingZeros(d.digits);
  }
  if (allZeroDigits(d.digits)) {
    d.digits = "0";
    d.negative = false;
  }
}

/// Digit string -> unsigned integer with overflow detection (the digits carry no sign).
bool digitsToUInt64(std::string_view digits, uint64_t& out) {
  uint64_t v = 0;
  for (char c : digits) {
    const uint64_t d = static_cast<uint64_t>(c - '0');
    if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

// ---- locale-independent text -> double ----------------------------------------------------------

/// Parses a plain decimal / scientific numeral ("-12.5e+3", "nan", "inf"). Never consults the C
/// locale, which a host application may have switched to one that uses ',' as decimal separator.
double parseDoubleC(const char* text) {
#if defined(TRINO_ODBC_HAVE_FP_TO_CHARS)
  const char* begin = text;
  bool negative = false;
  if (*begin == '+' || *begin == '-') {
    negative = *begin == '-';
    ++begin;
  }
  double value = 0.0;
  const auto res = std::from_chars(begin, begin + std::strlen(begin), value, std::chars_format::general);
  if (res.ec == std::errc::result_out_of_range) {
    // from_chars refuses to saturate; strtod semantics (0 on underflow, inf on overflow) are what
    // callers expect, so decide by the decimal magnitude of the leading nonzero digit.
    int pointPos = -1, firstNonZero = -1, idx = 0;
    const char* p = begin;
    for (; *p && *p != 'e' && *p != 'E'; ++p) {
      if (*p == '.') {
        pointPos = idx;
        continue;
      }
      if (*p >= '1' && *p <= '9' && firstNonZero < 0) firstNonZero = idx;
      ++idx;
    }
    if (pointPos < 0) pointPos = idx;
    const long exponent = *p ? std::strtol(p + 1, nullptr, 10) : 0;
    const long magnitude = firstNonZero < 0 ? -1 : static_cast<long>(pointPos - firstNonZero - 1) + exponent;
    value = magnitude > 0 ? std::numeric_limits<double>::infinity() : 0.0;
  } else if (res.ec != std::errc()) {
    value = 0.0;
  }
  return negative ? -value : value;
#elif defined(TRINO_ODBC_UNIX)
  static locale_t cLocale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
  if (cLocale) return strtod_l(text, nullptr, cLocale);
  return std::strtod(text, nullptr);
#else
  return std::strtod(text, nullptr);
#endif
}

// ---- shortest round-trip digits ----------------------------------------------------------------

/// Shortest decimal digits and exponent E such that d[0].d[1..] x 10^E round-trips to the value.
struct ShortestRepr {
  bool negative = false;
  std::string digits;  // no trailing zeros; "0" for zero
  int exponent = 0;    // scientific exponent of digits[0]
};

/// Splits printf/to_chars scientific output ("-1.2345e+20", "1e-07") into digits + exponent.
bool splitScientific(const char* text, ShortestRepr& out) {
  const char* p = text;
  out.negative = false;
  if (*p == '-') {
    out.negative = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }
  out.digits.clear();
  for (; *p && *p != 'e' && *p != 'E'; ++p) {
    if (*p >= '0' && *p <= '9') out.digits.push_back(*p);
    // Any other character is the decimal separator (locale dependent for printf): skip it.
  }
  int exp = 0;
  if (*p == 'e' || *p == 'E') {
    ++p;
    bool negExp = false;
    if (*p == '+' || *p == '-') {
      negExp = *p == '-';
      ++p;
    }
    if (*p < '0' || *p > '9') return false;
    for (; *p >= '0' && *p <= '9'; ++p) exp = exp * 10 + (*p - '0');
    if (negExp) exp = -exp;
  }
  // printf may emit a leading zero only for the value 0 itself; strip it defensively.
  size_t lead = 0;
  while (lead + 1 < out.digits.size() && out.digits[lead] == '0') ++lead;
  if (lead) {
    out.digits.erase(0, lead);
    exp -= static_cast<int>(lead);
  }
  while (out.digits.size() > 1 && out.digits.back() == '0') out.digits.pop_back();
  if (out.digits.empty()) out.digits = "0";
  out.exponent = out.digits == "0" ? 0 : exp;
  return true;
}

#if !defined(TRINO_ODBC_HAVE_FP_TO_CHARS)
/// Probe loop: the smallest %.Ne precision whose text parses back to the same value is the shortest
/// round-trip representation (17 significant digits always round-trip a double, 9 a float).
template <typename T>
bool shortestByProbe(T v, int maxPrecision, ShortestRepr& out) {
  char buf[64];
  for (int prec = 0; prec <= maxPrecision; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*e", prec, static_cast<double>(v));
    // A non-"C" process locale may print ',' as the decimal separator; normalise before parsing.
    for (char* c = buf; *c; ++c) {
      if (*c == ',') *c = '.';
    }
    if (static_cast<T>(parseDoubleC(buf)) == v || prec == maxPrecision) return splitScientific(buf, out);
  }
  return false;
}
#endif

bool shortestDouble(double v, ShortestRepr& out) {
#if defined(TRINO_ODBC_HAVE_FP_TO_CHARS)
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof buf - 1, v, std::chars_format::scientific);
  if (res.ec != std::errc()) return false;
  *res.ptr = '\0';
  return splitScientific(buf, out);
#else
  return shortestByProbe<double>(v, 16, out);
#endif
}

bool shortestFloat(float v, ShortestRepr& out) {
#if defined(TRINO_ODBC_HAVE_FP_TO_CHARS)
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof buf - 1, v, std::chars_format::scientific);
  if (res.ec != std::errc()) return false;
  *res.ptr = '\0';
  return splitScientific(buf, out);
#else
  return shortestByProbe<float>(v, 8, out);
#endif
}

/// Lays out shortest digits the same way on every platform: fixed notation for exponents in
/// [-4, 16] (so integers up to 17 digits print plainly), scientific "d.ddde+XX" otherwise.
std::string layoutShortest(const ShortestRepr& r) {
  std::string out;
  if (r.negative) out.push_back('-');
  if (r.digits == "0") {
    out.push_back('0');
    return out;
  }
  const int e = r.exponent;
  const std::string& d = r.digits;
  if (e >= -4 && e <= 16) {
    if (e >= 0) {
      const size_t intLen = static_cast<size_t>(e) + 1;
      if (d.size() <= intLen) {
        out += d;
        out.append(intLen - d.size(), '0');
      } else {
        out.append(d, 0, intLen);
        out.push_back('.');
        out.append(d, intLen, std::string::npos);
      }
    } else {
      out += "0.";
      out.append(static_cast<size_t>(-e - 1), '0');
      out += d;
    }
    return out;
  }
  out.push_back(d[0]);
  if (d.size() > 1) {
    out.push_back('.');
    out.append(d, 1, std::string::npos);
  }
  out.push_back('e');
  out.push_back(e < 0 ? '-' : '+');
  const int absExp = e < 0 ? -e : e;
  if (absExp < 10) out.push_back('0');
  out += std::to_string(absExp);
  return out;
}

template <typename T>
std::string nonFiniteText(T v) {
  if (std::isnan(v)) return "NaN";
  return v < 0 ? "-Infinity" : "Infinity";
}

}  // namespace

// ---- Decimal ------------------------------------------------------------------------------------

bool Decimal::isZero() const { return allZeroDigits(digits); }

std::string Decimal::toString() const {
  std::string out;
  const bool zero = isZero();
  if (negative && !zero) out.push_back('-');
  if (scale <= 0) {
    out += digits;
    if (scale < 0) out.append(static_cast<size_t>(-scale), '0');
    return out;
  }
  const size_t s = static_cast<size_t>(scale);
  if (digits.size() <= s) {
    out += "0.";
    out.append(s - digits.size(), '0');
    out += digits;
  } else {
    out.append(digits, 0, digits.size() - s);
    out.push_back('.');
    out.append(digits, digits.size() - s, std::string::npos);
  }
  return out;
}

bool parseDecimal(std::string_view s, Decimal& out) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  if (b == e) return false;

  Decimal d;
  size_t i = b;
  if (s[i] == '+' || s[i] == '-') {
    d.negative = s[i] == '-';
    ++i;
  }
  std::string digits;
  size_t intDigits = 0, fracDigits = 0;
  for (; i < e && s[i] >= '0' && s[i] <= '9'; ++i, ++intDigits) digits.push_back(s[i]);
  if (i < e && s[i] == '.') {
    ++i;
    for (; i < e && s[i] >= '0' && s[i] <= '9'; ++i, ++fracDigits) digits.push_back(s[i]);
  }
  if (intDigits + fracDigits == 0) return false;  // ".", "-", "e5"

  int64_t exponent = 0;
  if (i < e && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool negExp = false;
    if (i < e && (s[i] == '+' || s[i] == '-')) {
      negExp = s[i] == '-';
      ++i;
    }
    if (i >= e) return false;
    for (; i < e && s[i] >= '0' && s[i] <= '9'; ++i) {
      exponent = exponent * 10 + (s[i] - '0');
      if (exponent > 1000000) return false;  // absurd exponent; refuse rather than allocate gigabytes
    }
    if (negExp) exponent = -exponent;
  }
  if (i != e) return false;  // trailing garbage

  const int64_t scale = static_cast<int64_t>(fracDigits) - exponent;
  if (scale > 100000 || digits.size() + static_cast<size_t>(scale < 0 ? -scale : 0) > 100000) return false;
  d.digits = std::move(digits);
  d.scale = static_cast<int>(scale);
  canonicalize(d);
  out = std::move(d);
  return true;
}

bool rescaleDecimal(Decimal& d, int scale, bool round) {
  if (scale < 0) scale = 0;
  if (scale == d.scale) return false;
  if (scale > d.scale) {
    d.digits.append(static_cast<size_t>(scale - d.scale), '0');
    d.scale = scale;
    stripLeadingZeros(d.digits);
    return false;
  }
  const size_t drop = static_cast<size_t>(d.scale - scale);
  std::string dropped;
  if (drop >= d.digits.size()) {
    dropped = d.digits;
    d.digits = "0";
  } else {
    dropped = d.digits.substr(d.digits.size() - drop);
    d.digits.erase(d.digits.size() - drop);
  }
  d.scale = scale;
  const bool lost = !allZeroDigits(dropped);
  // The dropped block is right-aligned with `drop` positions; when it is shorter than `drop` its
  // leading digit is an implicit zero (value < 0.5 ulp) so no rounding applies.
  if (round && lost && dropped.size() == drop && dropped[0] >= '5') incrementDigits(d.digits);
  canonicalize(d);
  return lost;
}

bool decimalToNumeric(const Decimal& d, SQLSMALLINT precision, SQLSMALLINT scale, SQL_NUMERIC_STRUCT& out,
                      bool* fractionTruncated) {
  if (fractionTruncated) *fractionTruncated = false;
  // 39 digits is the most a 128-bit magnitude can hold; anything above is a caller bug, not overflow.
  if (precision < 1 || precision > 39 || scale < 0 || scale > precision) return false;

  Decimal v = d;
  canonicalize(v);
  const bool lost = rescaleDecimal(v, scale, /*round=*/true);
  if (fractionTruncated) *fractionTruncated = lost;
  if (!v.isZero() && v.digits.size() > static_cast<size_t>(precision)) return false;

  U128 mag;
  size_t pos = 0;
  size_t chunk = v.digits.size() % 9;
  if (chunk == 0) chunk = 9;
  while (pos < v.digits.size()) {
    uint32_t part = 0, mul = 1;
    for (size_t k = 0; k < chunk; ++k, ++pos) {
      part = part * 10 + static_cast<uint32_t>(v.digits[pos] - '0');
      mul *= 10;
    }
    if (mulAdd(mag, mul, part)) return false;
    chunk = 9;
  }

  std::memset(&out, 0, sizeof out);
  out.precision = static_cast<SQLCHAR>(precision);
  out.scale = static_cast<SQLSCHAR>(scale);
  out.sign = (v.negative && !v.isZero()) ? 0 : 1;
  for (int i = 0; i < 4; ++i) {
    out.val[i * 4 + 0] = static_cast<SQLCHAR>(mag.w[i] & 0xFFu);
    out.val[i * 4 + 1] = static_cast<SQLCHAR>((mag.w[i] >> 8) & 0xFFu);
    out.val[i * 4 + 2] = static_cast<SQLCHAR>((mag.w[i] >> 16) & 0xFFu);
    out.val[i * 4 + 3] = static_cast<SQLCHAR>((mag.w[i] >> 24) & 0xFFu);
  }
  return true;
}

bool numericToDecimal(const SQL_NUMERIC_STRUCT& in, Decimal& out) {
  U128 mag;
  for (int i = 0; i < 4; ++i) {
    mag.w[i] = static_cast<uint32_t>(in.val[i * 4 + 0]) | (static_cast<uint32_t>(in.val[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(in.val[i * 4 + 2]) << 16) | (static_cast<uint32_t>(in.val[i * 4 + 3]) << 24);
  }
  // Peel base-10^9 chunks off the low end; the first (most significant) chunk is printed unpadded.
  std::string chunks[5];
  int n = 0;
  while (!mag.isZero() && n < 5) {
    chunks[n++] = std::to_string(divMod(mag, kBase));
  }
  Decimal d;
  if (n == 0) {
    d.digits = "0";
  } else {
    d.digits = chunks[n - 1];
    for (int i = n - 2; i >= 0; --i) {
      d.digits.append(9 - chunks[i].size(), '0');
      d.digits += chunks[i];
    }
  }
  d.negative = in.sign == 0;
  d.scale = in.scale;
  canonicalize(d);
  out = std::move(d);
  return true;
}

bool decimalToInt64(const Decimal& d, int64_t& out, bool* fractionTruncated) {
  Decimal v = d;
  canonicalize(v);
  const bool lost = rescaleDecimal(v, 0, /*round=*/false);
  if (fractionTruncated) *fractionTruncated = lost;
  uint64_t mag = 0;
  if (!digitsToUInt64(v.digits, mag)) return false;
  if (v.negative) {
    if (mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u) return false;
    out = mag == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u
              ? std::numeric_limits<int64_t>::min()
              : -static_cast<int64_t>(mag);
    return true;
  }
  if (mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
  out = static_cast<int64_t>(mag);
  return true;
}

bool decimalToUInt64(const Decimal& d, uint64_t& out, bool* fractionTruncated) {
  Decimal v = d;
  canonicalize(v);
  const bool lost = rescaleDecimal(v, 0, /*round=*/false);
  if (fractionTruncated) *fractionTruncated = lost;
  if (v.negative && !v.isZero()) return false;
  return digitsToUInt64(v.digits, out);
}

double decimalToDouble(const Decimal& d) {
  const std::string text = d.toString();
  return parseDoubleC(text.c_str());
}

Decimal decimalFromInt64(int64_t v) {
  Decimal d;
  if (v < 0) {
    d.negative = true;
    // Negate through unsigned arithmetic so INT64_MIN does not overflow.
    d.digits = std::to_string(static_cast<uint64_t>(0) - static_cast<uint64_t>(v));
  } else {
    d.digits = std::to_string(v);
  }
  d.scale = 0;
  return d;
}

Decimal decimalFromDouble(double v) {
  Decimal d;
  if (!std::isfinite(v)) return d;
  ShortestRepr r;
  if (!shortestDouble(v, r)) return d;
  d.negative = r.negative;
  d.digits = r.digits;
  // digits[0].digits[1..] x 10^E  ==  digits x 10^(E - (len-1))
  d.scale = static_cast<int>(r.digits.size()) - 1 - r.exponent;
  canonicalize(d);
  return d;
}

std::string doubleToString(double v) {
  if (!std::isfinite(v)) return nonFiniteText(v);
  ShortestRepr r;
  if (!shortestDouble(v, r)) {
    // Unreachable in practice (buffer is ample); fall back to a verbose but correct rendering.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    for (char* c = buf; *c; ++c) {
      if (*c == ',') *c = '.';
    }
    return buf;
  }
  return layoutShortest(r);
}

std::string floatToString(float v) {
  if (!std::isfinite(v)) return nonFiniteText(v);
  ShortestRepr r;
  if (!shortestFloat(v, r)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    for (char* c = buf; *c; ++c) {
      if (*c == ',') *c = '.';
    }
    return buf;
  }
  return layoutShortest(r);
}

}  // namespace trino_odbc
