#pragma once
// IANA time zone database access used by the temporal conversions and the client (X-Trino-Time-Zone).
//
//  * Unix/macOS: TZif (v1/v2/v3) files from $TZDIR and the usual zoneinfo directories, including the
//    POSIX TZ footer rule for instants beyond the transition table.
//  * Windows: ICU (icu.dll, loaded dynamically) when available, otherwise the Windows registry rules
//    (GetTimeZoneInformationForYear) reached through a compiled-in CLDR Windows -> IANA table.
//  * Everywhere: "UTC", "Z", "GMT", "Etc/UTC", "Etc/GMT+5", fixed offsets "+HH:MM" and plain POSIX TZ
//    strings resolve without any files; Europe/Istanbul (the driver's default session zone) falls back
//    to a compiled-in +03:00 when no other source knows it.
//
// Parsed zones are cached for the life of the process; every method is thread-safe.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace trino_odbc {

class TimeZoneDb {
 public:
  static TimeZoneDb& global();

  /// Offset (seconds east of UTC) in effect at the given UTC instant. False when the zone is unknown.
  bool utcOffsetAtUtc(std::string_view zoneName, int64_t utcEpochSeconds, int32_t* offsetSeconds);
  /// Offset for a wall-clock instant expressed as epoch seconds "as if UTC" (resolves gaps/overlaps by
  /// choosing the earlier offset). False when unknown.
  bool utcOffsetAtLocal(std::string_view zoneName, int64_t localEpochSeconds, int32_t* offsetSeconds);
  bool isKnown(std::string_view zoneName);
  /// IANA name of the OS zone ("" if unknown): $TZ, /etc/localtime symlink, /etc/timezone,
  /// Windows: GetDynamicTimeZoneInformation + Windows->IANA table.
  static std::string systemZoneName();

 private:
  TimeZoneDb();
  ~TimeZoneDb();
  TimeZoneDb(const TimeZoneDb&) = delete;
  TimeZoneDb& operator=(const TimeZoneDb&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Compiled-in fixed offset used when a zone is found nowhere else (no TZif file, no ICU, no Windows
/// registry rule): currently Europe/Istanbul (+03:00, also Asia/Istanbul and Turkey). False otherwise.
bool fixedZoneFallback(std::string_view zoneName, int32_t* offsetSeconds);

/// Parses "+05:30", "-0800", "Z", "UTC" into offset seconds. False otherwise.
bool parseFixedOffset(std::string_view text, int32_t* offsetSeconds);

}  // namespace trino_odbc
