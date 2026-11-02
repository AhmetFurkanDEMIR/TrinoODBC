#pragma once
// Trino type -> ODBC SQL type mapping, column metadata, and the static SQLGetTypeInfo table.
#include "platform.h"
#include "core/config.h"
#include "types/trino_type.h"

#include <string>
#include <vector>

namespace trino_odbc {

/// Physical representation of a column inside a Page (see result/page.h).
enum class StorageKind : uint8_t { Bool, Int64, Double, String, Binary };

/// Everything ODBC wants to know about a column or a type.
struct OdbcTypeInfo {
  SQLSMALLINT conciseType = SQL_WVARCHAR;   // SQL_DESC_CONCISE_TYPE (SQL_TYPE_TIMESTAMP, SQL_WVARCHAR ...)
  SQLSMALLINT verboseType = SQL_WVARCHAR;   // SQL_DESC_TYPE (SQL_DATETIME for temporal, SQL_INTERVAL for intervals)
  SQLSMALLINT datetimeSubcode = 0;          // SQL_DESC_DATETIME_INTERVAL_CODE
  SQLULEN columnSize = 0;                   // SQL_DESC_LENGTH / SQLDescribeCol ColumnSize
  SQLLEN octetLength = 0;                   // SQL_DESC_OCTET_LENGTH (bytes of the transfer type)
  SQLLEN displaySize = 0;                   // SQL_DESC_DISPLAY_SIZE
  SQLSMALLINT decimalDigits = 0;            // scale / fractional seconds
  SQLSMALLINT precision = 0;                // SQL_DESC_PRECISION
  SQLINTEGER numPrecRadix = 0;              // 10 for exact numerics, 2 for approximate, 0 otherwise
  SQLSMALLINT nullable = SQL_NULLABLE;
  SQLSMALLINT searchable = SQL_PRED_SEARCHABLE;
  bool caseSensitive = false;
  bool unsignedAttr = false;                // true for non-numeric types (per spec)
  bool fixedPrecScale = false;
  bool autoIncrement = false;
  std::string typeName;                     // Trino type name ("varchar(10)")
  std::string localTypeName;
  std::string literalPrefix, literalSuffix; // "'" for strings, "DATE '" etc.
  std::string createParams;                 // SQLGetTypeInfo CREATE_PARAMS
  SQLSMALLINT minScale = 0, maxScale = 0;
  SQLSMALLINT defaultCType = SQL_C_WCHAR;   // SQL_C_DEFAULT target
  StorageKind storage = StorageKind::String;
  TypeCategory category = TypeCategory::Unknown;
  int temporalPrecision = 0;                // fractional digits carried by the Trino type
  bool intervalType = false;
  SQLSMALLINT intervalCode = 0;             // SQL_CODE_DAY_TO_SECOND ... when intervalType
};

/// Column metadata mapping for a Trino type (never throws; unknown -> text).
OdbcTypeInfo mapTrinoType(const TrinoType& type, const TypeMappingOptions& opts);
/// Storage used for a Trino type regardless of ODBC exposure.
StorageKind storageKindFor(TypeCategory category);
/// Rows for SQLGetTypeInfo(SQL_ALL_TYPES) in the spec-mandated order (by DATA_TYPE, then closeness).
std::vector<OdbcTypeInfo> odbcTypeInfoTable(const TypeMappingOptions& opts);
/// Default C type for a SQL type (SQL_C_DEFAULT resolution, ODBC Appendix D).
SQLSMALLINT defaultCTypeFor(SQLSMALLINT sqlType);
/// SQL_DESC_TYPE for a concise type (SQL_DATETIME / SQL_INTERVAL / itself).
SQLSMALLINT verboseTypeOf(SQLSMALLINT conciseType);
/// SQL_DESC_DATETIME_INTERVAL_CODE for a concise type (0 when none).
SQLSMALLINT datetimeSubcodeOf(SQLSMALLINT conciseType);
/// Inverse: concise type from verbose type + subcode.
SQLSMALLINT conciseTypeOf(SQLSMALLINT verboseType, SQLSMALLINT subcode);
/// True when `sqlType` is a valid ODBC SQL data type identifier (SQLBindParameter validation).
bool isValidSqlType(SQLSMALLINT sqlType);
/// True when `cType` is a valid ODBC C data type identifier (SQLBindCol / SQLGetData validation).
bool isValidCType(SQLSMALLINT cType);
/// Trino type to use in CAST(NULL AS ...) / literal typing for an ODBC SQL type.
std::string trinoTypeForSqlType(SQLSMALLINT sqlType, SQLULEN columnSize, SQLSMALLINT decimalDigits);
/// Human names for logs.
const char* sqlTypeName(SQLSMALLINT sqlType);
const char* cTypeName(SQLSMALLINT cType);
/// Whether a C type is a character/binary type retrievable in chunks by SQLGetData.
bool isChunkedCType(SQLSMALLINT cType);
/// Maps a legacy C type (SQL_C_DATE/TIME/TIMESTAMP) to its ODBC 3 equivalent, otherwise identity.
SQLSMALLINT normalizeCType(SQLSMALLINT cType);

}  // namespace trino_odbc
