// Unit tests for convert/temporal.{h,cpp}: Trino temporal text parsing/formatting, civil-day math,
// zone conversion and interval text <-> SQL_INTERVAL_STRUCT.
#include "unit/test_framework.h"

#include "convert/temporal.h"

#include <cstdio>
#include <string>

using namespace trino_odbc;

namespace {

/// Named IANA zones need the OS database; tests using them are skipped when it is absent.
bool haveZoneinfo() {
  std::FILE* f = std::fopen("/usr/share/zoneinfo/America/New_York", "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

}  // namespace

// ---- dates ------------------------------------------------------------------------------------

TEST_CASE("temporal: parseTrinoDate accepts Trino forms") {
  CivilDate d;
  REQUIRE(parseTrinoDate("2024-02-29", d));
  CHECK_EQ(d.year, 2024);
  CHECK_EQ(d.month, 2);
  CHECK_EQ(d.day, 29);

  REQUIRE(parseTrinoDate("  1970-01-01 ", d));
  CHECK_EQ(d.year, 1970);

  REQUIRE(parseTrinoDate("-0001-12-31", d));
  CHECK_EQ(d.year, -1);
  CHECK_EQ(d.month, 12);

  REQUIRE(parseTrinoDate("12345-06-07", d));
  CHECK_EQ(d.year, 12345);
  CHECK_EQ(d.day, 7);

  REQUIRE(parseTrinoDate("+2024-01-02", d));
  CHECK_EQ(d.year, 2024);
}

TEST_CASE("temporal: parseTrinoDate rejects invalid text") {
  CivilDate d;
  CHECK(!parseTrinoDate("2023-02-29", d));   // not a leap year
  CHECK(!parseTrinoDate("2024-13-01", d));
  CHECK(!parseTrinoDate("2024-00-10", d));
  CHECK(!parseTrinoDate("2024-04-31", d));
  CHECK(!parseTrinoDate("2024-04-01x", d));
  CHECK(!parseTrinoDate("2024/04/01", d));
  CHECK(!parseTrinoDate("", d));
  CHECK(!parseTrinoDate("2024-04-01 10:00:00", d));
}

TEST_CASE("temporal: isValidDate") {
  CHECK(isValidDate(2000, 2, 29));
  CHECK(!isValidDate(1900, 2, 29));
  CHECK(isValidDate(2024, 2, 29));
  CHECK(!isValidDate(2024, 2, 30));
  CHECK(isValidDate(-4, 2, 29));  // proleptic leap year
  CHECK(!isValidDate(2024, 0, 1));
  CHECK(!isValidDate(2024, 1, 0));
  CHECK(!isValidDate(2024, 12, 32));
}

TEST_CASE("temporal: daysFromCivil / civilFromDays round trip") {
  CHECK_EQ(daysFromCivil(1970, 1, 1), int64_t(0));
  CHECK_EQ(daysFromCivil(1969, 12, 31), int64_t(-1));
  CHECK_EQ(daysFromCivil(2000, 3, 1), int64_t(11017));
  CHECK_EQ(daysFromCivil(2024, 1, 1), int64_t(19723));
  CHECK_EQ(daysFromCivil(1, 1, 1), int64_t(-719162));
  CHECK_EQ(daysFromCivil(0, 1, 1), int64_t(-719528));

  int32_t y = 0;
  int m = 0, d = 0;
  civilFromDays(0, y, m, d);
  CHECK_EQ(y, 1970); CHECK_EQ(m, 1); CHECK_EQ(d, 1);
  civilFromDays(-1, y, m, d);
  CHECK_EQ(y, 1969); CHECK_EQ(m, 12); CHECK_EQ(d, 31);
  civilFromDays(19723, y, m, d);
  CHECK_EQ(y, 2024); CHECK_EQ(m, 1); CHECK_EQ(d, 1);

  // Every day over a few centuries (including negative years) must round-trip exactly.
  int mismatches = 0;
  for (int64_t days = daysFromCivil(-200, 1, 1); days <= daysFromCivil(2400, 12, 31); days += 7) {
    civilFromDays(days, y, m, d);
    if (!isValidDate(y, m, d) || daysFromCivil(y, m, d) != days) ++mismatches;
  }
  CHECK_EQ(mismatches, 0);
}

TEST_CASE("temporal: formatDate") {
  CHECK_EQ(formatDate(CivilDate{2024, 3, 9}), std::string("2024-03-09"));
  CHECK_EQ(formatDate(CivilDate{1, 1, 1}), std::string("0001-01-01"));
  CHECK_EQ(formatDate(CivilDate{-1, 12, 31}), std::string("-0001-12-31"));
  CHECK_EQ(formatDate(CivilDate{12345, 6, 7}), std::string("12345-06-07"));
  CivilDate d;
  REQUIRE(parseTrinoDate(formatDate(CivilDate{-44, 3, 15}), d));
  CHECK_EQ(d.year, -44);
}

// ---- times ------------------------------------------------------------------------------------

TEST_CASE("temporal: parseTrinoTime fractions and precision") {
  CivilTime t;
  int32_t off = 99;
  bool hasOff = true;
  REQUIRE(parseTrinoTime("12:34:56", t, &off, &hasOff));
  CHECK_EQ(t.hour, 12); CHECK_EQ(t.minute, 34); CHECK_EQ(t.second, 56);
  CHECK_EQ(t.nanos, int64_t(0));
  CHECK_EQ(t.precision, 0);
  CHECK(!hasOff);
  CHECK_EQ(off, 0);

  REQUIRE(parseTrinoTime("12:34:56.123", t, nullptr, nullptr));
  CHECK_EQ(t.nanos, int64_t(123000000));
  CHECK_EQ(t.precision, 3);

  REQUIRE(parseTrinoTime("00:00:00.000001", t, nullptr, nullptr));
  CHECK_EQ(t.nanos, int64_t(1000));
  CHECK_EQ(t.precision, 6);

  REQUIRE(parseTrinoTime("23:59:59.123456789", t, nullptr, nullptr));
  CHECK_EQ(t.nanos, int64_t(123456789));
  CHECK_EQ(t.precision, 9);

  // Picosecond digits are truncated to nanoseconds but the precision remembers them.
  REQUIRE(parseTrinoTime("23:59:59.123456789999", t, nullptr, nullptr));
  CHECK_EQ(t.nanos, int64_t(123456789));
  CHECK_EQ(t.precision, 12);
}

TEST_CASE("temporal: parseTrinoTime offsets") {
  CivilTime t;
  int32_t off = 0;
  bool hasOff = false;
  REQUIRE(parseTrinoTime("12:34:56.123456+05:30", t, &off, &hasOff));
  CHECK(hasOff);
  CHECK_EQ(off, 5 * 3600 + 30 * 60);
  CHECK_EQ(t.precision, 6);

  REQUIRE(parseTrinoTime("12:34:56-08:00", t, &off, &hasOff));
  CHECK(hasOff);
  CHECK_EQ(off, -8 * 3600);

  REQUIRE(parseTrinoTime("12:34:56 +0100", t, &off, &hasOff));
  CHECK_EQ(off, 3600);

  REQUIRE(parseTrinoTime("12:34:56Z", t, &off, &hasOff));
  CHECK(hasOff);
  CHECK_EQ(off, 0);

  REQUIRE(parseTrinoTime("12:34:56+05", t, &off, &hasOff));
  CHECK_EQ(off, 5 * 3600);
}

TEST_CASE("temporal: parseTrinoTime rejects invalid text") {
  CivilTime t;
  CHECK(!parseTrinoTime("24:00:00", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:60:00", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:00:60", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:00", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:00:00.", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:00:00+25:00", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("12:00:00 America/New_York", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("abc", t, nullptr, nullptr));
  CHECK(!parseTrinoTime("", t, nullptr, nullptr));
}

TEST_CASE("temporal: formatTime fraction digits") {
  CivilTime t;
  t.hour = 7; t.minute = 8; t.second = 9; t.nanos = 123456789;
  CHECK_EQ(formatTime(t, 0), std::string("07:08:09"));
  CHECK_EQ(formatTime(t, 3), std::string("07:08:09.123"));
  CHECK_EQ(formatTime(t, 6), std::string("07:08:09.123456"));
  CHECK_EQ(formatTime(t, 9), std::string("07:08:09.123456789"));
  CHECK_EQ(formatTime(t, 12), std::string("07:08:09.123456789000"));
  t.nanos = 5;
  CHECK_EQ(formatTime(t, 9), std::string("07:08:09.000000005"));
  CHECK_EQ(formatTime(t, 3), std::string("07:08:09.000"));
}

// ---- timestamps -------------------------------------------------------------------------------

TEST_CASE("temporal: parseTrinoTimestamp plain") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.123", ts));
  CHECK_EQ(ts.date.year, 2024);
  CHECK_EQ(ts.date.month, 3);
  CHECK_EQ(ts.date.day, 10);
  CHECK_EQ(ts.time.hour, 2);
  CHECK_EQ(ts.time.minute, 30);
  CHECK_EQ(ts.time.nanos, int64_t(123000000));
  CHECK_EQ(ts.time.precision, 3);
  CHECK(!ts.hasZone);

  REQUIRE(parseTrinoTimestamp("2024-03-10T02:30:00", ts));  // ISO separator tolerated
  CHECK_EQ(ts.time.hour, 2);
  CHECK(!ts.hasZone);
}

TEST_CASE("temporal: parseTrinoTimestamp zones") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.123456 America/New_York", ts));
  CHECK(ts.hasZone);
  CHECK(!ts.zoneIsOffset);
  CHECK_EQ(ts.zoneName, std::string("America/New_York"));
  CHECK_EQ(ts.time.precision, 6);

  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.123456 +02:00", ts));
  CHECK(ts.hasZone);
  CHECK(ts.zoneIsOffset);
  CHECK_EQ(ts.offsetSeconds, 7200);

  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00-05:30", ts));
  CHECK(ts.zoneIsOffset);
  CHECK_EQ(ts.offsetSeconds, -(5 * 3600 + 30 * 60));

  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 UTC", ts));
  CHECK(ts.hasZone);
  CHECK(!ts.zoneIsOffset);
  CHECK_EQ(ts.zoneName, std::string("UTC"));

  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 Etc/GMT+5", ts));
  CHECK_EQ(ts.zoneName, std::string("Etc/GMT+5"));

  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00Z", ts));
  CHECK(ts.zoneIsOffset);
  CHECK_EQ(ts.offsetSeconds, 0);
}

TEST_CASE("temporal: parseTrinoTimestamp rejects invalid text") {
  ParsedTimestamp ts;
  CHECK(!parseTrinoTimestamp("2024-03-10", ts));
  CHECK(!parseTrinoTimestamp("2024-03-10 25:00:00", ts));
  CHECK(!parseTrinoTimestamp("2024-02-30 10:00:00", ts));
  CHECK(!parseTrinoTimestamp("2024-03-10 10:00:00 Bad Zone", ts));
  CHECK(!parseTrinoTimestamp("2024-03-10 10:00:00America/New_York", ts));
  CHECK(parseTrinoTimestamp("2024-03-10 10:00:00 ", ts));  // trailing space is trimmed => valid
  CHECK(!parseTrinoTimestamp("garbage", ts));
}

TEST_CASE("temporal: timestampToUtc with fixed offsets") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.5 +02:00", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 1), std::string("2024-03-10 00:30:00.5"));
  CHECK(ts.hasZone);
  CHECK(ts.zoneIsOffset);
  CHECK_EQ(ts.offsetSeconds, 0);
  CHECK_EQ(ts.time.precision, 1);

  // Crossing a year boundary backwards and forwards.
  REQUIRE(parseTrinoTimestamp("2024-01-01 01:00:00 +05:00", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2023-12-31 20:00:00"));

  REQUIRE(parseTrinoTimestamp("2023-12-31 22:00:00 -05:30", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-01-01 03:30:00"));

  // No zone: untouched and reported as success.
  REQUIRE(parseTrinoTimestamp("2024-06-01 12:00:00", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-06-01 12:00:00"));
  CHECK(!ts.hasZone);
}

TEST_CASE("temporal: timestampToUtc with built-in zone names") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-06-01 12:00:00.123 Etc/UTC", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 3), std::string("2024-06-01 12:00:00.123"));

  REQUIRE(parseTrinoTimestamp("2024-06-01 12:00:00 UTC", ts));
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-06-01 12:00:00"));
}

TEST_CASE("temporal: timestampToUtc unknown zone leaves the value untouched") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-06-01 12:00:00 Nowhere/Land", ts));
  CHECK(!timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-06-01 12:00:00"));
  CHECK(ts.hasZone);
  CHECK(!ts.zoneIsOffset);
  CHECK_EQ(ts.zoneName, std::string("Nowhere/Land"));
}

TEST_CASE("temporal: timestampToUtc with a named zone (needs zoneinfo)") {
  if (!haveZoneinfo()) return;
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-01-15 12:00:00 America/New_York", ts));  // EST, UTC-5
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-01-15 17:00:00"));

  REQUIRE(parseTrinoTimestamp("2024-07-15 12:00:00 America/New_York", ts));  // EDT, UTC-4
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-07-15 16:00:00"));

  REQUIRE(parseTrinoTimestamp("2024-07-15 12:00:00 Europe/Istanbul", ts));  // UTC+3
  REQUIRE(timestampToUtc(ts));
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-07-15 09:00:00"));
}

TEST_CASE("temporal: timestampToZone renders in Europe/Istanbul") {
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.123 +02:00", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 3), std::string("2024-03-10 03:30:00.123"));
  CHECK(ts.zoneIsOffset);
  CHECK_EQ(ts.offsetSeconds, 3 * 3600);
  // Day and year rollover.
  REQUIRE(parseTrinoTimestamp("2023-12-31 22:00:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-01-01 01:00:00"));
  // Fixed-offset target and a value without zone (untouched).
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 +02:00", ts));
  CHECK(timestampToZone(ts, "-03:30") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-03-09 21:00:00"));
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-03-10 02:30:00"));
}

TEST_CASE("temporal: timestampToZone with named zones (needs zoneinfo)") {
  if (!haveZoneinfo()) return;
  ParsedTimestamp ts;
  // Spring-forward gap in New York: the earlier offset (EST) applies -> 07:30 UTC -> 10:30 Istanbul.
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00.123 America/New_York", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 3), std::string("2024-03-10 10:30:00.123"));
  // Europe/Istanbul before 2016 still had DST (EEST +03:00 in summer 2015, EET +02:00 in winter).
  REQUIRE(parseTrinoTimestamp("2015-01-15 12:00:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2015-01-15 14:00:00"));
  // A DST session zone: Berlin summer (+02:00) and winter (+01:00).
  REQUIRE(parseTrinoTimestamp("2024-07-15 12:00:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Berlin") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-07-15 14:00:00"));
  REQUIRE(parseTrinoTimestamp("2024-01-15 12:00:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Berlin") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-01-15 13:00:00"));
  // Berlin fall-back hour: 00:30 UTC on 2024-10-27 is 02:30 CEST; 01:30 UTC is 02:30 CET.
  REQUIRE(parseTrinoTimestamp("2024-10-27 00:30:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Berlin") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-10-27 02:30:00"));
  CHECK_EQ(ts.offsetSeconds, 7200);
  REQUIRE(parseTrinoTimestamp("2024-10-27 01:30:00 UTC", ts));
  CHECK(timestampToZone(ts, "Europe/Berlin") == ZoneConversion::Ok);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-10-27 02:30:00"));
  CHECK_EQ(ts.offsetSeconds, 3600);
}

TEST_CASE("temporal: timestampToZone with unresolvable zones") {
  ParsedTimestamp ts;
  // Unknown target zone: converted to UTC instead.
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 +02:00", ts));
  CHECK(timestampToZone(ts, "Nowhere/Land") == ZoneConversion::UnknownTargetZone);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-03-10 00:30:00"));
  CHECK_EQ(ts.offsetSeconds, 0);
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 +02:00", ts));
  CHECK(timestampToZone(ts, "") == ZoneConversion::UnknownTargetZone);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-03-10 00:30:00"));
  // Unknown value zone: untouched.
  REQUIRE(parseTrinoTimestamp("2024-03-10 02:30:00 Nowhere/Land", ts));
  CHECK(timestampToZone(ts, "Europe/Istanbul") == ZoneConversion::UnknownValueZone);
  CHECK_EQ(formatTimestamp(ts.date, ts.time, 0), std::string("2024-03-10 02:30:00"));
  CHECK_EQ(ts.zoneName, std::string("Nowhere/Land"));
}

TEST_CASE("temporal: timeToZone") {
  CivilTime t;
  REQUIRE(parseTrinoTime("12:34:56.5", t, nullptr, nullptr));
  CHECK(timeToZone(t, 5 * 3600 + 1800, "Europe/Istanbul"));  // +05:30 -> +03:00
  CHECK_EQ(formatTime(t, 1), std::string("10:04:56.5"));
  REQUIRE(parseTrinoTime("22:00:00", t, nullptr, nullptr));
  CHECK(timeToZone(t, 0, "+03:00"));
  CHECK_EQ(formatTime(t, 0), std::string("01:00:00"));
  REQUIRE(parseTrinoTime("12:00:00", t, nullptr, nullptr));
  CHECK(!timeToZone(t, 2 * 3600, "Nowhere/Land"));  // unknown -> UTC
  CHECK_EQ(formatTime(t, 0), std::string("10:00:00"));
}

TEST_CASE("temporal: formatTimestamp") {
  CivilDate d{2024, 12, 5};
  CivilTime t;
  t.hour = 1; t.minute = 2; t.second = 3; t.nanos = 400000000;
  CHECK_EQ(formatTimestamp(d, t, 0), std::string("2024-12-05 01:02:03"));
  CHECK_EQ(formatTimestamp(d, t, 3), std::string("2024-12-05 01:02:03.400"));
  ParsedTimestamp ts;
  REQUIRE(parseTrinoTimestamp(formatTimestamp(d, t, 6), ts));
  CHECK_EQ(ts.time.nanos, int64_t(400000000));
  CHECK_EQ(ts.time.precision, 6);
}

// ---- intervals --------------------------------------------------------------------------------

TEST_CASE("temporal: interval year to month parse/format") {
  SQL_INTERVAL_STRUCT v;
  REQUIRE(parseTrinoIntervalYearMonth("1-2", v));
  CHECK_EQ(static_cast<int>(v.interval_type), static_cast<int>(SQL_IS_YEAR_TO_MONTH));
  CHECK_EQ(v.interval_sign, SQL_FALSE);
  CHECK_EQ(static_cast<unsigned>(v.intval.year_month.year), 1u);
  CHECK_EQ(static_cast<unsigned>(v.intval.year_month.month), 2u);
  CHECK_EQ(formatIntervalYearMonth(v), std::string("1-2"));

  REQUIRE(parseTrinoIntervalYearMonth("-10-11", v));
  CHECK_EQ(v.interval_sign, SQL_TRUE);
  CHECK_EQ(static_cast<unsigned>(v.intval.year_month.year), 10u);
  CHECK_EQ(static_cast<unsigned>(v.intval.year_month.month), 11u);
  CHECK_EQ(formatIntervalYearMonth(v), std::string("-10-11"));

  REQUIRE(parseTrinoIntervalYearMonth(" 0-0 ", v));
  CHECK_EQ(formatIntervalYearMonth(v), std::string("0-0"));

  CHECK(!parseTrinoIntervalYearMonth("1", v));
  CHECK(!parseTrinoIntervalYearMonth("1-", v));
  CHECK(!parseTrinoIntervalYearMonth("1-2-3", v));
  CHECK(!parseTrinoIntervalYearMonth("a-b", v));
  CHECK(!parseTrinoIntervalYearMonth("99999999999-0", v));
}

TEST_CASE("temporal: interval day to second parse/format") {
  SQL_INTERVAL_STRUCT v;
  REQUIRE(parseTrinoIntervalDaySecond("3 04:05:06.789", v));
  CHECK_EQ(static_cast<int>(v.interval_type), static_cast<int>(SQL_IS_DAY_TO_SECOND));
  CHECK_EQ(v.interval_sign, SQL_FALSE);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.day), 3u);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.hour), 4u);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.minute), 5u);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.second), 6u);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.fraction), 789000000u);  // nanoseconds
  CHECK_EQ(formatIntervalDaySecond(v), std::string("3 04:05:06.789"));

  REQUIRE(parseTrinoIntervalDaySecond("-0 00:00:00.000", v));
  CHECK_EQ(v.interval_sign, SQL_TRUE);
  CHECK_EQ(formatIntervalDaySecond(v), std::string("-0 00:00:00.000"));

  REQUIRE(parseTrinoIntervalDaySecond("1 02:03:04", v));
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.fraction), 0u);
  CHECK_EQ(formatIntervalDaySecond(v), std::string("1 02:03:04.000"));

  REQUIRE(parseTrinoIntervalDaySecond("1 02:03:04.123456", v));
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.fraction), 123456000u);
  CHECK_EQ(formatIntervalDaySecond(v), std::string("1 02:03:04.123456"));

  REQUIRE(parseTrinoIntervalDaySecond("1 02:03:04.123456789", v));
  CHECK_EQ(formatIntervalDaySecond(v), std::string("1 02:03:04.123456789"));

  REQUIRE(parseTrinoIntervalDaySecond("12:00:00", v));  // day omitted
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.day), 0u);
  CHECK_EQ(static_cast<unsigned>(v.intval.day_second.hour), 12u);

  CHECK(!parseTrinoIntervalDaySecond("3 04:05", v));
  CHECK(!parseTrinoIntervalDaySecond("3 04:65:00", v));
  CHECK(!parseTrinoIntervalDaySecond("3 04:05:06.", v));
  CHECK(!parseTrinoIntervalDaySecond("x 04:05:06", v));
  CHECK(!parseTrinoIntervalDaySecond("", v));
}
