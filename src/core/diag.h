#pragma once
// ODBC diagnostics area attached to every handle.
#include "platform.h"
#include "util/errors.h"

#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

struct DiagRecord {
  std::string sqlstate;      // 5 chars
  SQLINTEGER native = 0;
  std::string message;       // already prefixed ([Trino][ODBC Driver]...)
  SQLLEN rowNumber = SQL_NO_ROW_NUMBER;
  SQLINTEGER columnNumber = SQL_NO_COLUMN_NUMBER;
  std::string serverName;
  std::string connectionName;
  bool isWarning() const { return sqlstate.size() >= 2 && sqlstate[0] == '0' && sqlstate[1] == '1'; }
};

/// Diagnostic area. Records are kept ordered as the spec requires: errors before warnings, and
/// within a class in insertion order. Header fields (return code, row count, dynamic function ...)
/// live here as well.
class Diag {
 public:
  void clear();
  bool empty() const { return records_.empty(); }
  size_t count() const { return records_.size(); }
  const std::vector<DiagRecord>& records() const { return records_; }
  const DiagRecord* record(size_t index1) const;  // 1-based; nullptr when out of range

  /// Adds an already formatted record. Returns SQL_ERROR or SQL_SUCCESS_WITH_INFO according to the class.
  SQLRETURN add(DiagRecord rec);
  /// Driver-originated error/warning. `message` is raw text; the vendor prefix is added here.
  SQLRETURN addError(std::string_view sqlstate, std::string_view message, SQLINTEGER native = 0,
                     SQLLEN row = SQL_NO_ROW_NUMBER, SQLINTEGER column = SQL_NO_COLUMN_NUMBER);
  SQLRETURN addWarning(std::string_view sqlstate, std::string_view message, SQLINTEGER native = 0,
                       SQLLEN row = SQL_NO_ROW_NUMBER, SQLINTEGER column = SQL_NO_COLUMN_NUMBER);
  /// Server-originated error (message gets the [Trino] component and native = errorCode).
  SQLRETURN addServerError(const TrinoQueryError& e);
  /// Maps any DriverError subclass to a record. Returns SQL_ERROR / SQL_SUCCESS_WITH_INFO.
  SQLRETURN addException(const DriverError& e);
  SQLRETURN addException(const std::exception& e);  // HY000

  // Header fields
  SQLRETURN returnCode = SQL_SUCCESS;
  SQLLEN rowCount = -1;             // SQL_DIAG_ROW_COUNT
  SQLLEN cursorRowCount = -1;       // SQL_DIAG_CURSOR_ROW_COUNT
  std::string dynamicFunction;      // SQL_DIAG_DYNAMIC_FUNCTION
  SQLINTEGER dynamicFunctionCode = SQL_DIAG_UNKNOWN_STATEMENT;
  std::string serverName;           // used for SQL_DIAG_SERVER_NAME on new records
  std::string connectionName;

  /// "[Trino][ODBC Driver] text"
  static std::string formatDriverMessage(std::string_view text);
  /// "[Trino][ODBC Driver][Trino] text"
  static std::string formatServerMessage(std::string_view text);
  /// SQLSTATE for a Trino error name / type (see ARCHITECTURE.md §8).
  static const char* sqlstateForTrinoError(std::string_view errorName, std::string_view errorType);
  /// SQL_DIAG_DYNAMIC_FUNCTION_CODE + text for a SQL statement (SELECT_CURSOR, INSERT, ...).
  static void classifyDynamicFunction(std::string_view sql, std::string& name, SQLINTEGER& code);

 private:
  std::vector<DiagRecord> records_;
};

/// Combine two return codes keeping the most severe (SQL_ERROR > SQL_NEED_DATA > SQL_NO_DATA > SWI > SUCCESS).
SQLRETURN mergeReturnCode(SQLRETURN a, SQLRETURN b);

}  // namespace trino_odbc
