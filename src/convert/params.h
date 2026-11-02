#pragma once
// Bound parameter (application C buffer) -> Trino SQL literal rendering for EXECUTE ... USING.
#include "platform.h"
#include "convert/convert.h"
#include "types/trino_type.h"

#include <optional>
#include <string>

namespace trino_odbc {

/// Snapshot of one parameter value for one parameter set.
struct ParamValue {
  SQLSMALLINT cType = SQL_C_DEFAULT;      // application C type (SQL_C_DEFAULT resolved by renderer)
  SQLSMALLINT sqlType = SQL_UNKNOWN_TYPE; // SQLBindParameter ParameterType
  SQLULEN columnSize = 0;
  SQLSMALLINT decimalDigits = 0;
  const void* data = nullptr;             // application buffer (already offset for the param set)
  SQLLEN length = SQL_NTS;                // *StrLen_or_IndPtr semantics (SQL_NTS, byte count, SQL_NULL_DATA)
  SQLLEN bufferLength = 0;                // SQLBindParameter BufferLength
  bool isNull = false;
  const std::string* daeData = nullptr;   // accumulated SQLPutData bytes when data-at-exec was used
  bool isDefault = false;                 // SQL_DEFAULT_PARAM
};

/// Renders `value` as a Trino literal / typed expression. `serverType` (from DESCRIBE INPUT) wins
/// over `sqlType` for typing when present. Throws DriverError (22003 / 22018 / 22008 / 07006 / HY105).
std::string renderParameterLiteral(const ParamValue& value, const ConvertOptions& opts,
                                   const std::optional<TrinoType>& serverType);

}  // namespace trino_odbc
