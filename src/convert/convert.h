#pragma once
// Column value -> application C buffer conversions (SQLGetData / SQLFetch) and the chunked
// retrieval bookkeeping.
#include "platform.h"
#include "core/config.h"
#include "core/diag.h"
#include "result/row_source.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trino_odbc {

struct ConvertOptions {
  AnsiEncoding ansi = AnsiEncoding::Utf8;
  TimestampTzMode timestampTz = TimestampTzMode::Session;
  TimeTzMode timeTz = TimeTzMode::String;
  IntervalMode interval = IntervalMode::String;
  /// Effective session zone (IANA name or offset): target of the Session modes and the zone of
  /// "today" for time -> timestamp conversions. Empty = unknown (Session modes fall back to UTC).
  std::string sessionTimeZone = kDefaultSessionTimeZone;
};

/// Where and how to write the converted value.
struct ConvertTarget {
  SQLSMALLINT cType = SQL_C_DEFAULT;
  SQLPOINTER buffer = nullptr;
  SQLLEN bufferLength = 0;
  SQLLEN* strLenOrInd = nullptr;
  // SQL_C_NUMERIC precision/scale from the ARD record (0 / unset => derive from the column).
  SQLSMALLINT numericPrecision = 0;
  SQLSMALLINT numericScale = 0;
  bool numericScaleSet = false;
};

/// Per-column progress for SQLGetData chunking within the current row.
struct GetDataState {
  std::vector<size_t> offsets;   // bytes of the source already returned; SIZE_MAX = fully returned
  void reset(size_t columnCount) { offsets.assign(columnCount, 0); }
};

/// Converts cell (data,row) described by `col` into `target`.
/// - `chunkOffset` non-null enables SQLGetData chunking: on entry it is the source byte offset already
///   delivered, on exit it is advanced; when all data has been delivered before the call the function
///   returns SQL_NO_DATA. Bound columns (SQLFetch) pass nullptr.
/// - Diagnostics (01004, 01S07, 22003, 22018, 22008, 07006, HY003 ...) are added to `diag` with the
///   supplied row/column numbers. Returns SQL_SUCCESS / SQL_SUCCESS_WITH_INFO / SQL_ERROR / SQL_NO_DATA.
SQLRETURN convertToC(const ResultColumn& col, const Column& data, size_t row, const ConvertTarget& target,
                     const ConvertOptions& opts, size_t* chunkOffset, Diag& diag, SQLLEN rowNumber,
                     SQLINTEGER columnNumber);

/// Text rendering of a cell as the driver exposes it for SQL_C_CHAR/WCHAR (e.g. timestamp tz mode
/// applied, booleans as "1"/"0", doubles shortest round-trip). Used by conversions and by tests.
std::string cellToText(const ResultColumn& col, const Column& data, size_t row, const ConvertOptions& opts);

}  // namespace trino_odbc
