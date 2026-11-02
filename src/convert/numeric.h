#pragma once
// Exact decimal arithmetic for ODBC: decimal text <-> SQL_NUMERIC_STRUCT (128-bit little-endian
// magnitude) and shortest round-trip rendering of doubles/floats. Everything here is exact; no value
// ever passes through a binary floating-point type unless the function name says "Double".
#include "platform.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace trino_odbc {

/// Exact decimal: value = (negative ? -1 : 1) * digits * 10^-scale ; digits has no leading zeros ("0" for zero).
/// Canonical form: scale >= 0 (exponent forms are expanded on parse) and zero is never negative.
struct Decimal {
  bool negative = false;
  std::string digits = "0";
  int scale = 0;

  bool isZero() const;
  std::string toString() const;  /* canonical, e.g. "-12.340" keeps scale */
};

/// Parses "[+-]digits[.digits]" and "1E5"/"1.5e-3" forms (surrounding ASCII whitespace ignored).
bool parseDecimal(std::string_view s, Decimal& out);
/// Rescales to `scale` digits; returns true if fractional digits were lost (rounding half away from zero
/// when round=true, else truncate).
bool rescaleDecimal(Decimal& d, int scale, bool round);
/// -> SQL_NUMERIC_STRUCT with the requested precision/scale. Returns false on overflow (precision exceeded);
/// *fractionTruncated set when digits were dropped. Rounds half away from zero when reducing the scale.
bool decimalToNumeric(const Decimal& d, SQLSMALLINT precision, SQLSMALLINT scale, SQL_NUMERIC_STRUCT& out,
                      bool* fractionTruncated);
/// SQL_NUMERIC_STRUCT (sign 1 = positive, val little-endian) -> exact decimal. Always succeeds for a
/// well-formed struct; a negative scale is expanded into trailing zeros.
bool numericToDecimal(const SQL_NUMERIC_STRUCT& in, Decimal& out);
/// Integer conversion (truncates toward zero): false on overflow; *fractionTruncated when nonzero fraction dropped.
bool decimalToInt64(const Decimal& d, int64_t& out, bool* fractionTruncated);
bool decimalToUInt64(const Decimal& d, uint64_t& out, bool* fractionTruncated);
/// Correctly rounded (locale independent) conversion to the nearest double.
double decimalToDouble(const Decimal& d);
Decimal decimalFromInt64(int64_t v);
/// Shortest round-trip representation; NaN/Infinity yield zero (callers check std::isfinite first).
Decimal decimalFromDouble(double v);
/// Shortest round-trip text for doubles/floats ("1.5", "1e+300", "NaN", "Infinity").
/// Fixed notation is used when the decimal exponent is in [-4, 16], scientific ("d.ddde+XX") otherwise;
/// the output is identical on every platform and never depends on the C locale.
std::string doubleToString(double v);
std::string floatToString(float v);

}  // namespace trino_odbc
