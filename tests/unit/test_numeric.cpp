// Unit tests for convert/numeric: exact decimal <-> SQL_NUMERIC_STRUCT and shortest float text.
#include "unit/test_framework.h"

#include "convert/numeric.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace trino_odbc;

namespace {

std::string dec(std::string_view text) {
  Decimal d;
  REQUIRE(parseDecimal(text, d));
  return d.toString();
}

/// Round-trips text through SQL_NUMERIC_STRUCT at the given precision/scale and back to text.
std::string viaNumeric(std::string_view text, SQLSMALLINT precision, SQLSMALLINT scale, bool* truncated = nullptr) {
  Decimal d;
  REQUIRE(parseDecimal(text, d));
  SQL_NUMERIC_STRUCT num;
  bool lost = false;
  REQUIRE(decimalToNumeric(d, precision, scale, num, &lost));
  if (truncated) *truncated = lost;
  CHECK_EQ(static_cast<int>(num.precision), static_cast<int>(precision));
  CHECK_EQ(static_cast<int>(num.scale), static_cast<int>(scale));
  Decimal back;
  REQUIRE(numericToDecimal(num, back));
  return back.toString();
}

bool numericOverflows(std::string_view text, SQLSMALLINT precision, SQLSMALLINT scale) {
  Decimal d;
  REQUIRE(parseDecimal(text, d));
  SQL_NUMERIC_STRUCT num;
  return !decimalToNumeric(d, precision, scale, num, nullptr);
}

}  // namespace

TEST_CASE("numeric: parseDecimal canonical forms") {
  CHECK_EQ(dec("0"), std::string("0"));
  CHECK_EQ(dec("-0"), std::string("0"));
  CHECK_EQ(dec("-0.00"), std::string("0.00"));
  CHECK_EQ(dec("000123"), std::string("123"));
  CHECK_EQ(dec("+12.340"), std::string("12.340"));
  CHECK_EQ(dec("-12.340"), std::string("-12.340"));
  CHECK_EQ(dec(".5"), std::string("0.5"));
  CHECK_EQ(dec("5."), std::string("5"));
  CHECK_EQ(dec("  42  "), std::string("42"));
  CHECK_EQ(dec("1E5"), std::string("100000"));
  CHECK_EQ(dec("1.5e-3"), std::string("0.0015"));
  CHECK_EQ(dec("1.5e+3"), std::string("1500"));
  CHECK_EQ(dec("12.5E1"), std::string("125"));
  CHECK_EQ(dec("-1e0"), std::string("-1"));
  CHECK_EQ(dec("0.000"), std::string("0.000"));

  Decimal d;
  CHECK(parseDecimal("123.456", d));
  CHECK_EQ(d.digits, std::string("123456"));
  CHECK_EQ(d.scale, 3);
  CHECK(!d.negative);
  CHECK(parseDecimal("-0.001", d));
  CHECK_EQ(d.digits, std::string("1"));
  CHECK_EQ(d.scale, 3);
  CHECK(d.negative);
}

TEST_CASE("numeric: parseDecimal rejects garbage") {
  Decimal d;
  CHECK(!parseDecimal("", d));
  CHECK(!parseDecimal("   ", d));
  CHECK(!parseDecimal("-", d));
  CHECK(!parseDecimal(".", d));
  CHECK(!parseDecimal("1.2.3", d));
  CHECK(!parseDecimal("12a", d));
  CHECK(!parseDecimal("1e", d));
  CHECK(!parseDecimal("1e+", d));
  CHECK(!parseDecimal("e5", d));
  CHECK(!parseDecimal("1 2", d));
  CHECK(!parseDecimal("NaN", d));
  CHECK(!parseDecimal("0x10", d));
  CHECK(!parseDecimal("1e99999999", d));
}

TEST_CASE("numeric: toString / isZero") {
  Decimal d;
  CHECK(d.isZero());
  CHECK_EQ(d.toString(), std::string("0"));
  d.digits = "5";
  d.scale = 3;
  CHECK_EQ(d.toString(), std::string("0.005"));
  d.negative = true;
  CHECK_EQ(d.toString(), std::string("-0.005"));
  d.digits = "12345";
  d.scale = 5;
  CHECK_EQ(d.toString(), std::string("-0.12345"));
  d.scale = 0;
  CHECK_EQ(d.toString(), std::string("-12345"));
  d.digits = "0";
  d.scale = 2;
  CHECK(d.isZero());
  CHECK_EQ(d.toString(), std::string("0.00"));  // negative zero never prints a sign
}

TEST_CASE("numeric: rescaleDecimal rounding and truncation") {
  Decimal d;
  REQUIRE(parseDecimal("12.345", d));
  CHECK(!rescaleDecimal(d, 5, true));
  CHECK_EQ(d.toString(), std::string("12.34500"));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("12.35"));  // half away from zero

  REQUIRE(parseDecimal("-12.345", d));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("-12.35"));

  REQUIRE(parseDecimal("-12.345", d));
  CHECK(rescaleDecimal(d, 2, false));
  CHECK_EQ(d.toString(), std::string("-12.34"));  // truncate toward zero

  REQUIRE(parseDecimal("12.344999", d));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("12.34"));

  REQUIRE(parseDecimal("9.999", d));
  CHECK(rescaleDecimal(d, 0, true));
  CHECK_EQ(d.toString(), std::string("10"));  // carry propagates to a new digit

  REQUIRE(parseDecimal("0.0004", d));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("0.00"));

  REQUIRE(parseDecimal("-0.004", d));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("0.00"));
  CHECK(!d.negative);

  REQUIRE(parseDecimal("0.005", d));
  CHECK(rescaleDecimal(d, 2, true));
  CHECK_EQ(d.toString(), std::string("0.01"));

  REQUIRE(parseDecimal("1.500", d));
  CHECK(!rescaleDecimal(d, 1, true));  // only zeros dropped: nothing lost
  CHECK_EQ(d.toString(), std::string("1.5"));

  REQUIRE(parseDecimal("123", d));
  CHECK(!rescaleDecimal(d, 0, true));
  CHECK_EQ(d.toString(), std::string("123"));
}

TEST_CASE("numeric: SQL_NUMERIC_STRUCT layout") {
  Decimal d;
  REQUIRE(parseDecimal("-1.5", d));
  SQL_NUMERIC_STRUCT num;
  REQUIRE(decimalToNumeric(d, 10, 2, num, nullptr));
  CHECK_EQ(static_cast<int>(num.sign), 0);  // 0 = negative
  CHECK_EQ(static_cast<int>(num.precision), 10);
  CHECK_EQ(static_cast<int>(num.scale), 2);
  CHECK_EQ(static_cast<int>(num.val[0]), 150);  // 1.50 -> 150 little-endian
  for (int i = 1; i < 16; ++i) CHECK_EQ(static_cast<int>(num.val[i]), 0);

  REQUIRE(parseDecimal("256", d));
  REQUIRE(decimalToNumeric(d, 5, 0, num, nullptr));
  CHECK_EQ(static_cast<int>(num.sign), 1);
  CHECK_EQ(static_cast<int>(num.val[0]), 0);
  CHECK_EQ(static_cast<int>(num.val[1]), 1);

  // 2^64 = 18446744073709551616 occupies exactly the 9th byte.
  REQUIRE(parseDecimal("18446744073709551616", d));
  REQUIRE(decimalToNumeric(d, 20, 0, num, nullptr));
  for (int i = 0; i < 8; ++i) CHECK_EQ(static_cast<int>(num.val[i]), 0);
  CHECK_EQ(static_cast<int>(num.val[8]), 1);

  // Zero is positive and all-zero magnitude.
  REQUIRE(parseDecimal("-0.0", d));
  REQUIRE(decimalToNumeric(d, 3, 1, num, nullptr));
  CHECK_EQ(static_cast<int>(num.sign), 1);
  for (int i = 0; i < 16; ++i) CHECK_EQ(static_cast<int>(num.val[i]), 0);
}

TEST_CASE("numeric: round trips at precision/scale boundaries") {
  CHECK_EQ(viaNumeric("0", 1, 0), std::string("0"));
  CHECK_EQ(viaNumeric("9", 1, 0), std::string("9"));
  CHECK_EQ(viaNumeric("-9", 1, 0), std::string("-9"));
  CHECK_EQ(viaNumeric("0.9", 1, 1), std::string("0.9"));
  CHECK_EQ(viaNumeric("12.34", 10, 2), std::string("12.34"));
  CHECK_EQ(viaNumeric("-12.34", 10, 2), std::string("-12.34"));
  CHECK_EQ(viaNumeric("12.34", 10, 4), std::string("12.3400"));
  CHECK_EQ(viaNumeric("9223372036854775807", 19, 0), std::string("9223372036854775807"));
  CHECK_EQ(viaNumeric("-9223372036854775808", 19, 0), std::string("-9223372036854775808"));
  CHECK_EQ(viaNumeric("18446744073709551615", 20, 0), std::string("18446744073709551615"));
  CHECK_EQ(viaNumeric("18446744073709551616", 20, 0), std::string("18446744073709551616"));
  // 38 nines: the largest decimal(38,0).
  const std::string nines38(38, '9');
  CHECK_EQ(viaNumeric(nines38, 38, 0), nines38);
  CHECK_EQ(viaNumeric("-" + nines38, 38, 0), "-" + nines38);
  // All 38 digits after the point.
  CHECK_EQ(viaNumeric("0." + nines38, 38, 38), "0." + nines38);
  CHECK_EQ(viaNumeric("-0." + nines38, 38, 38), "-0." + nines38);
  // Mixed split with distinct digits so byte order mistakes would show.
  CHECK_EQ(viaNumeric("12345678901234567890.123456789012345678", 38, 18),
           std::string("12345678901234567890.123456789012345678"));
  CHECK_EQ(viaNumeric("1", 38, 37), "1." + std::string(37, '0'));
  // 2^128 - 1 is the largest magnitude the struct can carry (39 digits).
  CHECK_EQ(viaNumeric("340282366920938463463374607431768211455", 39, 0),
           std::string("340282366920938463463374607431768211455"));
  // Powers of 10^9 boundaries.
  CHECK_EQ(viaNumeric("1000000000", 10, 0), std::string("1000000000"));
  CHECK_EQ(viaNumeric("999999999", 9, 0), std::string("999999999"));
  CHECK_EQ(viaNumeric("1000000000000000000", 19, 0), std::string("1000000000000000000"));
  CHECK_EQ(viaNumeric("100000000000000000000000000000000000000", 39, 0),
           std::string("100000000000000000000000000000000000000"));
}

TEST_CASE("numeric: decimalToNumeric rounding and fraction flag") {
  bool lost = false;
  CHECK_EQ(viaNumeric("12.345", 10, 2, &lost), std::string("12.35"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("-12.345", 10, 2, &lost), std::string("-12.35"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("12.344", 10, 2, &lost), std::string("12.34"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("12.340", 10, 2, &lost), std::string("12.34"));
  CHECK(!lost);
  CHECK_EQ(viaNumeric("0.5", 10, 0, &lost), std::string("1"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("-0.5", 10, 0, &lost), std::string("-1"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("0.4999", 10, 0, &lost), std::string("0"));
  CHECK(lost);
  CHECK_EQ(viaNumeric("-0.001", 10, 2, &lost), std::string("0.00"));
  CHECK(lost);
}

TEST_CASE("numeric: decimalToNumeric overflow detection") {
  CHECK(numericOverflows("10", 1, 0));
  CHECK(numericOverflows("9.99", 1, 0));   // rounds to 10 -> two digits
  CHECK(!numericOverflows("9.49", 1, 0));
  CHECK(numericOverflows("100", 4, 2));    // 100.00 needs 5 digits
  CHECK(!numericOverflows("99.99", 4, 2));
  CHECK(numericOverflows("99.995", 4, 2)); // rounds to 100.00
  CHECK(numericOverflows(std::string(39, '9'), 38, 0));
  CHECK(!numericOverflows(std::string(38, '9'), 38, 0));
  CHECK(numericOverflows("1", 38, 38));    // 1.000...(38) has 39 digits
  CHECK(numericOverflows("340282366920938463463374607431768211456", 39, 0));  // 2^128
  // Invalid precision/scale combinations are refused rather than producing garbage.
  CHECK(numericOverflows("1", 0, 0));
  CHECK(numericOverflows("1", 40, 0));
  CHECK(numericOverflows("1", 5, 6));
  CHECK(numericOverflows("1", 5, -1));
}

TEST_CASE("numeric: numericToDecimal edge cases") {
  SQL_NUMERIC_STRUCT num;
  std::memset(&num, 0, sizeof num);
  num.precision = 10;
  num.scale = 0;
  num.sign = 1;
  Decimal d;
  REQUIRE(numericToDecimal(num, d));
  CHECK(d.isZero());
  CHECK_EQ(d.toString(), std::string("0"));

  // All 16 bytes 0xFF = 2^128 - 1.
  std::memset(num.val, 0xFF, sizeof num.val);
  num.precision = 39;
  REQUIRE(numericToDecimal(num, d));
  CHECK_EQ(d.toString(), std::string("340282366920938463463374607431768211455"));

  // Negative zero from an application is still zero.
  std::memset(num.val, 0, sizeof num.val);
  num.sign = 0;
  REQUIRE(numericToDecimal(num, d));
  CHECK(!d.negative);

  // Negative scale means multiply by 10^-scale.
  num.val[0] = 5;
  num.sign = 1;
  num.scale = -3;
  REQUIRE(numericToDecimal(num, d));
  CHECK_EQ(d.toString(), std::string("5000"));
  CHECK_EQ(d.scale, 0);

  // Sign byte other than 1 means negative.
  num.scale = 1;
  num.sign = 0;
  REQUIRE(numericToDecimal(num, d));
  CHECK_EQ(d.toString(), std::string("-0.5"));
}

TEST_CASE("numeric: integer conversions") {
  Decimal d;
  int64_t i = 0;
  uint64_t u = 0;
  bool lost = false;

  REQUIRE(parseDecimal("9223372036854775807", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, std::numeric_limits<int64_t>::max());
  CHECK(!lost);
  REQUIRE(parseDecimal("9223372036854775808", d));
  CHECK(!decimalToInt64(d, i, &lost));
  CHECK(decimalToUInt64(d, u, &lost));
  CHECK_EQ(u, static_cast<uint64_t>(9223372036854775808ULL));
  REQUIRE(parseDecimal("-9223372036854775808", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, std::numeric_limits<int64_t>::min());
  REQUIRE(parseDecimal("-9223372036854775809", d));
  CHECK(!decimalToInt64(d, i, &lost));
  REQUIRE(parseDecimal("18446744073709551615", d));
  CHECK(decimalToUInt64(d, u, &lost));
  CHECK_EQ(u, std::numeric_limits<uint64_t>::max());
  REQUIRE(parseDecimal("18446744073709551616", d));
  CHECK(!decimalToUInt64(d, u, &lost));

  REQUIRE(parseDecimal("12.90", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, static_cast<int64_t>(12));  // truncates toward zero
  CHECK(lost);
  REQUIRE(parseDecimal("-12.90", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, static_cast<int64_t>(-12));
  CHECK(lost);
  REQUIRE(parseDecimal("12.00", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, static_cast<int64_t>(12));
  CHECK(!lost);
  REQUIRE(parseDecimal("-0.5", d));
  CHECK(decimalToUInt64(d, u, &lost));  // truncates to zero: not negative
  CHECK_EQ(u, static_cast<uint64_t>(0));
  CHECK(lost);
  REQUIRE(parseDecimal("-1", d));
  CHECK(!decimalToUInt64(d, u, &lost));
  REQUIRE(parseDecimal("0.000001", d));
  CHECK(decimalToInt64(d, i, &lost));
  CHECK_EQ(i, static_cast<int64_t>(0));
  CHECK(lost);

  CHECK_EQ(decimalFromInt64(0).toString(), std::string("0"));
  CHECK_EQ(decimalFromInt64(-1).toString(), std::string("-1"));
  CHECK_EQ(decimalFromInt64(std::numeric_limits<int64_t>::min()).toString(), std::string("-9223372036854775808"));
  CHECK_EQ(decimalFromInt64(std::numeric_limits<int64_t>::max()).toString(), std::string("9223372036854775807"));
}

TEST_CASE("numeric: decimal <-> double") {
  Decimal d;
  REQUIRE(parseDecimal("1.5", d));
  CHECK_EQ(decimalToDouble(d), 1.5);
  REQUIRE(parseDecimal("-0.1", d));
  CHECK_EQ(decimalToDouble(d), -0.1);
  REQUIRE(parseDecimal("123456789012345678901234567890", d));
  CHECK_EQ(decimalToDouble(d), 1.2345678901234568e29);
  REQUIRE(parseDecimal("0.30000000000000004", d));
  CHECK_EQ(decimalToDouble(d), 0.1 + 0.2);
  REQUIRE(parseDecimal("1e400", d));
  CHECK(std::isinf(decimalToDouble(d)));
  REQUIRE(parseDecimal("1e-400", d));
  CHECK_EQ(decimalToDouble(d), 0.0);
  REQUIRE(parseDecimal("0", d));
  CHECK_EQ(decimalToDouble(d), 0.0);

  CHECK_EQ(decimalFromDouble(1.5).toString(), std::string("1.5"));
  CHECK_EQ(decimalFromDouble(-0.1).toString(), std::string("-0.1"));
  CHECK_EQ(decimalFromDouble(100.0).toString(), std::string("100"));
  CHECK_EQ(decimalFromDouble(1e300).toString(), "1" + std::string(300, '0'));
  CHECK_EQ(decimalFromDouble(0.1 + 0.2).toString(), std::string("0.30000000000000004"));
  CHECK_EQ(decimalFromDouble(0.0).toString(), std::string("0"));
  CHECK_EQ(decimalFromDouble(-0.0).toString(), std::string("0"));
  CHECK_EQ(decimalFromDouble(1e-7).toString(), std::string("0.0000001"));
  CHECK(decimalFromDouble(std::numeric_limits<double>::quiet_NaN()).isZero());
  CHECK(decimalFromDouble(std::numeric_limits<double>::infinity()).isZero());
}

TEST_CASE("numeric: doubleToString shortest round trip") {
  CHECK_EQ(doubleToString(1.5), std::string("1.5"));
  CHECK_EQ(doubleToString(0.1), std::string("0.1"));
  CHECK_EQ(doubleToString(0.1 + 0.2), std::string("0.30000000000000004"));
  CHECK_EQ(doubleToString(100.0), std::string("100"));
  CHECK_EQ(doubleToString(-2.0), std::string("-2"));
  CHECK_EQ(doubleToString(0.0), std::string("0"));
  CHECK_EQ(doubleToString(-0.0), std::string("-0"));
  CHECK_EQ(doubleToString(123456.789), std::string("123456.789"));
  CHECK_EQ(doubleToString(1e300), std::string("1e+300"));
  CHECK_EQ(doubleToString(-1e300), std::string("-1e+300"));
  CHECK_EQ(doubleToString(1e16), std::string("10000000000000000"));
  CHECK_EQ(doubleToString(1e17), std::string("1e+17"));
  CHECK_EQ(doubleToString(1.25e17), std::string("1.25e+17"));
  CHECK_EQ(doubleToString(9007199254740992.0), std::string("9007199254740992"));
  CHECK_EQ(doubleToString(0.0001), std::string("0.0001"));
  CHECK_EQ(doubleToString(0.00001), std::string("1e-05"));
  CHECK_EQ(doubleToString(1.5e-7), std::string("1.5e-07"));
  CHECK_EQ(doubleToString(5e-324), std::string("5e-324"));
  CHECK_EQ(doubleToString(std::numeric_limits<double>::max()), std::string("1.7976931348623157e+308"));
  CHECK_EQ(doubleToString(std::numeric_limits<double>::min()), std::string("2.2250738585072014e-308"));
  CHECK_EQ(doubleToString(std::numeric_limits<double>::quiet_NaN()), std::string("NaN"));
  CHECK_EQ(doubleToString(std::numeric_limits<double>::infinity()), std::string("Infinity"));
  CHECK_EQ(doubleToString(-std::numeric_limits<double>::infinity()), std::string("-Infinity"));
  CHECK_EQ(doubleToString(1.0 / 3.0), std::string("0.3333333333333333"));
  CHECK_EQ(doubleToString(2.0 / 3.0), std::string("0.6666666666666666"));

  // Every rendering must parse back to the identical double.
  const double samples[] = {1.5, 0.1, 0.1 + 0.2, 1e300, 1e-300, 123456.789, 5e-324, 1.0 / 3.0,
                            std::numeric_limits<double>::max(), 4.35, 2.675, 1e23, 8.41e21, 1.7e-9};
  for (double v : samples) {
    Decimal d;
    REQUIRE(parseDecimal(doubleToString(v), d));
    CHECK_EQ(decimalToDouble(d), v);
  }
}

TEST_CASE("numeric: floatToString shortest round trip") {
  CHECK_EQ(floatToString(1.5f), std::string("1.5"));
  CHECK_EQ(floatToString(0.1f), std::string("0.1"));  // float shortest, not the double expansion
  CHECK_EQ(floatToString(3.14159f), std::string("3.14159"));
  CHECK_EQ(floatToString(100.0f), std::string("100"));
  CHECK_EQ(floatToString(1e30f), std::string("1e+30"));
  CHECK_EQ(floatToString(16777216.0f), std::string("16777216"));
  CHECK_EQ(floatToString(std::numeric_limits<float>::max()), std::string("3.4028235e+38"));
  CHECK_EQ(floatToString(std::numeric_limits<float>::quiet_NaN()), std::string("NaN"));
  CHECK_EQ(floatToString(-std::numeric_limits<float>::infinity()), std::string("-Infinity"));
  const float samples[] = {1.5f, 0.1f, 3.14159f, 1e30f, 1e-30f, 0.3f, 2.5e-7f, 1.17549435e-38f};
  for (float v : samples) {
    Decimal d;
    REQUIRE(parseDecimal(floatToString(v), d));
    CHECK_EQ(static_cast<float>(decimalToDouble(d)), v);
  }
}
