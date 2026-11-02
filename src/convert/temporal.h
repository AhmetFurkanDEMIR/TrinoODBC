#pragma once
// Civil date/time parsing and formatting for Trino's textual temporal values, plus interval text
// <-> SQL_INTERVAL_STRUCT. Shared by convert (SQL -> C), params (C -> literal) and catalog code.
//
// Conventions:
//  * CivilTime::nanos holds nanoseconds (0..999'999'999); Trino digits beyond the 9th are truncated by
//    the parsers and CivilTime::precision records how many fractional digits the text carried.
//  * SQL_INTERVAL_STRUCT day_second.fraction produced/consumed here is in nanoseconds (the same unit
//    SQL_TIMESTAMP_STRUCT uses); scaling to an interval seconds precision is the caller's business.
//  * All fields of SQL_INTERVAL_STRUCT are magnitudes; interval_sign carries the sign.
#include "platform.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace trino_odbc {

struct CivilDate { int32_t year = 1970; int month = 1; int day = 1; };
struct CivilTime { int hour = 0, minute = 0, second = 0; int64_t nanos = 0; int precision = 0; };
struct ParsedTimestamp {
  CivilDate date; CivilTime time;
  bool hasZone = false;          // "... America/New_York" or "... +05:30"
  bool zoneIsOffset = false;     // true when the zone is a fixed offset
  int32_t offsetSeconds = 0;     // valid when zoneIsOffset
  std::string zoneName;          // IANA name when !zoneIsOffset
};
bool parseTrinoDate(std::string_view s, CivilDate& out);                    // YYYY-MM-DD (year may be negative / >4 digits)
bool parseTrinoTime(std::string_view s, CivilTime& out, int32_t* offsetSeconds /*nullable*/, bool* hasOffset /*nullable*/); // HH:MM:SS[.f{1,12}][±HH:MM]
bool parseTrinoTimestamp(std::string_view s, ParsedTimestamp& out);         // YYYY-MM-DD HH:MM:SS[.f...][ zone]
int64_t daysFromCivil(int32_t y, int m, int d);                              // days since 1970-01-01
void civilFromDays(int64_t days, int32_t& y, int& m, int& d);
bool isValidDate(int32_t y, int m, int d);
/// Today's date in `sessionTimeZone` (IANA name or fixed offset); the OS local zone when empty or unknown.
/// ODBC time -> timestamp conversions (both directions) take their date part from here.
CivilDate currentCivilDate(std::string_view sessionTimeZone);
/// Converts a zoned timestamp to UTC wall clock (uses TimeZoneDb). False when the zone is unknown (value untouched).
bool timestampToUtc(ParsedTimestamp& ts);
/// Outcome of timestampToZone.
enum class ZoneConversion {
  Ok,                 // converted (or the value carried no zone)
  UnknownValueZone,   // the value's own zone cannot be resolved: value untouched (wall clock as sent)
  UnknownTargetZone,  // `zone` is empty or cannot be resolved: value converted to UTC instead
};
/// Converts a zoned timestamp to the wall clock of `zone` (IANA name or fixed offset, e.g. the
/// session time zone). On success ts carries the target offset (zoneIsOffset, offsetSeconds).
ZoneConversion timestampToZone(ParsedTimestamp& ts, std::string_view zone);
/// Shifts a time-of-day with offset `offsetSeconds` to `zone`, using the zone's offset at the current
/// instant (a time with time zone has no date). False when `zone` is empty/unknown: then the time is
/// shifted to UTC instead.
bool timeToZone(CivilTime& t, int32_t offsetSeconds, std::string_view zone);
std::string formatDate(const CivilDate& d);                                   // YYYY-MM-DD
std::string formatTime(const CivilTime& t, int fractionDigits);              // HH:MM:SS[.fff]
std::string formatTimestamp(const CivilDate& d, const CivilTime& t, int fractionDigits);
bool parseTrinoIntervalYearMonth(std::string_view s, SQL_INTERVAL_STRUCT& out);  // "[-]Y-M"
bool parseTrinoIntervalDaySecond(std::string_view s, SQL_INTERVAL_STRUCT& out);  // "[-]D HH:MM:SS.fff"
std::string formatIntervalYearMonth(const SQL_INTERVAL_STRUCT& v);
std::string formatIntervalDaySecond(const SQL_INTERVAL_STRUCT& v);

}  // namespace trino_odbc
