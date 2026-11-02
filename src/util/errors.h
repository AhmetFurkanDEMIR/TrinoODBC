#pragma once
// Exception types used inside the driver. They never cross the C ABI: src/odbc/*.cpp catches every
// one of them and turns it into a diagnostic record on the handle.
#include "platform.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace trino_odbc {

// Frequently used SQLSTATEs. Keep names aligned with the ODBC spec wording.
namespace sqlstate {
constexpr const char* kGeneralWarning = "01000";
constexpr const char* kStringTruncated = "01004";
constexpr const char* kOptionValueChanged = "01S02";
constexpr const char* kErrorInRow = "01S01";
constexpr const char* kFractionalTruncation = "01S07";
constexpr const char* kNoDataForAttribute = "01S00";
constexpr const char* kInvalidDescriptorIndex = "07009";
constexpr const char* kRestrictedDataType = "07006";
constexpr const char* kWrongNumberOfParams = "07002";
constexpr const char* kInvalidUseOfDefaultParam = "07S01";
constexpr const char* kUnableToConnect = "08001";
constexpr const char* kConnectionInUse = "08002";
constexpr const char* kNotConnected = "08003";
constexpr const char* kServerRejectedConnection = "08004";
constexpr const char* kCommunicationLinkFailure = "08S01";
constexpr const char* kInvalidCursorState = "24000";
constexpr const char* kInvalidTransactionState = "25000";
constexpr const char* kInvalidAuth = "28000";
constexpr const char* kInvalidCursorName = "34000";
constexpr const char* kDuplicateCursorName = "3C000";
constexpr const char* kCatalogNotFound = "3D000";
constexpr const char* kSchemaNotFound = "3F000";
constexpr const char* kStringRightTruncation = "22001";
constexpr const char* kIndicatorRequired = "22002";
constexpr const char* kNumericOutOfRange = "22003";
constexpr const char* kInvalidDatetimeFormat = "22007";
constexpr const char* kDatetimeOverflow = "22008";
constexpr const char* kDivisionByZero = "22012";
constexpr const char* kIntervalOverflow = "22015";
constexpr const char* kInvalidCharacterValue = "22018";
constexpr const char* kInvalidEscapeCharacter = "22019";
constexpr const char* kInvalidEscapeSequence = "22025";
constexpr const char* kInvalidArgument = "22023";
constexpr const char* kSyntaxError = "42000";
constexpr const char* kTableNotFound = "42S02";
constexpr const char* kColumnNotFound = "42S22";
constexpr const char* kTableExists = "42S01";
constexpr const char* kIndexNotFound = "42S12";
constexpr const char* kPermissionDenied = "42501";
constexpr const char* kTypeMismatch = "42804";
constexpr const char* kAmbiguousName = "42702";
constexpr const char* kGeneralError = "HY000";
constexpr const char* kMemoryAllocation = "HY001";
constexpr const char* kInvalidAppBufferType = "HY003";
constexpr const char* kInvalidSqlDataType = "HY004";
constexpr const char* kInvalidNullPointer = "HY009";
constexpr const char* kFunctionSequence = "HY010";
constexpr const char* kAttributeCannotBeSetNow = "HY011";
constexpr const char* kInvalidTransactionOperation = "HY012";
constexpr const char* kMemoryManagement = "HY013";
constexpr const char* kLimitOnHandles = "HY014";
constexpr const char* kCannotModifyIrd = "HY016";
constexpr const char* kInvalidUseOfDescriptor = "HY017";
constexpr const char* kServerDeclinedCancel = "HY018";
constexpr const char* kNonCharSentInPieces = "HY019";
constexpr const char* kInconsistentDescriptor = "HY021";
constexpr const char* kInvalidAttributeValue = "HY024";
constexpr const char* kInvalidStringOrBufferLength = "HY090";
constexpr const char* kInvalidDescriptorFieldId = "HY091";
constexpr const char* kInvalidAttribute = "HY092";
constexpr const char* kInvalidParameterNumber = "HY093";
constexpr const char* kFunctionTypeOutOfRange = "HY095";
constexpr const char* kInformationTypeOutOfRange = "HY096";
constexpr const char* kColumnTypeOutOfRange = "HY097";
constexpr const char* kScopeTypeOutOfRange = "HY098";
constexpr const char* kNullableTypeOutOfRange = "HY099";
constexpr const char* kUniquenessOptionOutOfRange = "HY100";
constexpr const char* kAccuracyOptionOutOfRange = "HY101";
constexpr const char* kInvalidRetrievalCode = "HY103";
constexpr const char* kInvalidPrecisionOrScale = "HY104";
constexpr const char* kInvalidParameterType = "HY105";
constexpr const char* kFetchTypeOutOfRange = "HY106";
constexpr const char* kRowValueOutOfRange = "HY107";
constexpr const char* kInvalidCursorPosition = "HY109";
constexpr const char* kInvalidDriverCompletion = "HY110";
constexpr const char* kInvalidBookmarkValue = "HY111";
constexpr const char* kOptionalFeatureNotImplemented = "HYC00";
constexpr const char* kTimeout = "HYT00";
constexpr const char* kConnectionTimeout = "HYT01";
constexpr const char* kCancelled = "HY008";
constexpr const char* kDriverNotCapable = "IM001";
constexpr const char* kDataSourceNotFound = "IM002";
constexpr const char* kDialogFailed = "IM008";
constexpr const char* kNoDataSourceOrDriver = "IM007";
constexpr const char* kSetupError = "S1000";
}  // namespace sqlstate

/// Base driver error carrying an ODBC SQLSTATE, message text and optional native code / position.
class DriverError : public std::exception {
 public:
  DriverError(std::string sqlstate, std::string message, SQLINTEGER native = 0)
      : sqlstate_(std::move(sqlstate)), message_(std::move(message)), native_(native) {}

  const char* what() const noexcept override { return message_.c_str(); }
  const std::string& sqlstate() const noexcept { return sqlstate_; }
  const std::string& message() const noexcept { return message_; }
  SQLINTEGER native() const noexcept { return native_; }
  SQLLEN rowNumber() const noexcept { return row_; }
  SQLINTEGER columnNumber() const noexcept { return column_; }
  DriverError& atRow(SQLLEN row) { row_ = row; return *this; }
  DriverError& atColumn(SQLINTEGER col) { column_ = col; return *this; }
  /// True for SQLSTATE class 01 (warning) — used when an exception carries SQL_SUCCESS_WITH_INFO semantics.
  bool isWarning() const noexcept { return sqlstate_.size() >= 2 && sqlstate_[0] == '0' && sqlstate_[1] == '1'; }

 protected:
  std::string sqlstate_;
  std::string message_;
  SQLINTEGER native_;
  SQLLEN row_ = SQL_NO_ROW_NUMBER;
  SQLINTEGER column_ = SQL_NO_COLUMN_NUMBER;
};

/// Error reported by the Trino server inside the `error` object of a query response.
class TrinoQueryError : public DriverError {
 public:
  TrinoQueryError(std::string sqlstate, std::string message, int errorCode, std::string errorName,
                  std::string errorType, std::string queryId, int line = 0, int column = 0)
      : DriverError(std::move(sqlstate), std::move(message), errorCode),
        errorName_(std::move(errorName)), errorType_(std::move(errorType)), queryId_(std::move(queryId)),
        line_(line), col_(column) {}
  const std::string& errorName() const noexcept { return errorName_; }
  const std::string& errorType() const noexcept { return errorType_; }
  const std::string& queryId() const noexcept { return queryId_; }
  int line() const noexcept { return line_; }
  int column() const noexcept { return col_; }

 private:
  std::string errorName_, errorType_, queryId_;
  int line_, col_;
};

/// Transport-level failure (curl error, non-2xx HTTP status, TLS failure, timeout).
class HttpError : public DriverError {
 public:
  HttpError(std::string sqlstate, std::string message, long httpStatus, std::string url, int curlCode = 0)
      : DriverError(std::move(sqlstate), std::move(message), static_cast<SQLINTEGER>(httpStatus)),
        httpStatus_(httpStatus), url_(std::move(url)), curlCode_(curlCode) {}
  long httpStatus() const noexcept { return httpStatus_; }
  const std::string& url() const noexcept { return url_; }
  int curlCode() const noexcept { return curlCode_; }

 private:
  long httpStatus_;
  std::string url_;
  int curlCode_;
};

/// Raised when SQLCancel (or a timeout) aborted the operation.
class CancelledError : public DriverError {
 public:
  CancelledError() : DriverError(sqlstate::kCancelled, "Operation canceled") {}
  explicit CancelledError(std::string message) : DriverError(sqlstate::kCancelled, std::move(message)) {}
};

}  // namespace trino_odbc
