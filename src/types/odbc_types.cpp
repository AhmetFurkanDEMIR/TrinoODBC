// Trino type -> ODBC SQL type mapping (docs/design/TYPE-MAPPING-RULES.md is the spec), the static
// SQLGetTypeInfo table, and the ODBC type-identifier helpers used by the API layer.
#include "types/odbc_types.h"

#include "util/log.h"

#include <algorithm>
#include <limits>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "types";

SQLSMALLINT clampSmall(int64_t v) {
  const int64_t hi = std::numeric_limits<SQLSMALLINT>::max();
  return static_cast<SQLSMALLINT>(std::min<int64_t>(std::max<int64_t>(v, 0), hi));
}

SQLLEN clampLen(uint64_t v) {
  const uint64_t hi = static_cast<uint64_t>(std::numeric_limits<SQLLEN>::max());
  return static_cast<SQLLEN>(std::min<uint64_t>(v, hi));
}

SQLULEN clampULen(int64_t v) {
  const uint64_t hi = static_cast<uint64_t>(std::numeric_limits<SQLULEN>::max());
  return static_cast<SQLULEN>(std::min<uint64_t>(static_cast<uint64_t>(std::max<int64_t>(v, 0)), hi));
}

/// Bytes needed to transfer `chars` characters of a (wide) character column. The ODBC spec writes
/// this as 2*n for SQL_WCHAR, which assumes a 2-byte SQLWCHAR; iODBC uses 4 bytes, so the real
/// width is used instead of the literal 2.
SQLLEN charOctetLength(int64_t chars, bool unicode) {
  const uint64_t width = unicode ? sizeof(SQLWCHAR) : 1;
  return clampLen(static_cast<uint64_t>(std::max<int64_t>(chars, 0)) * width);
}

/// Fills the fields shared by every character-like exposure ((W)CHAR/(W)VARCHAR/(W)LONGVARCHAR).
void setCharacter(OdbcTypeInfo& info, const TypeMappingOptions& opts, SQLSMALLINT narrowType, SQLSMALLINT wideType,
                  int64_t chars) {
  info.conciseType = opts.unicode ? wideType : narrowType;
  info.verboseType = info.conciseType;
  info.datetimeSubcode = 0;
  info.columnSize = clampULen(chars);
  info.octetLength = charOctetLength(chars, opts.unicode);
  info.displaySize = clampLen(static_cast<uint64_t>(std::max<int64_t>(chars, 0)));
  info.decimalDigits = 0;
  info.precision = clampSmall(chars);
  info.numPrecRadix = 0;
  info.defaultCType = opts.unicode ? SQL_C_WCHAR : SQL_C_CHAR;
}

/// Exact / approximate numeric exposure.
void setNumeric(OdbcTypeInfo& info, SQLSMALLINT sqlType, SQLULEN columnSize, SQLLEN octetLength, SQLLEN displaySize,
                SQLSMALLINT decimalDigits, SQLINTEGER radix, SQLSMALLINT defaultC) {
  info.conciseType = sqlType;
  info.verboseType = sqlType;
  info.datetimeSubcode = 0;
  info.columnSize = columnSize;
  info.octetLength = octetLength;
  info.displaySize = displaySize;
  info.decimalDigits = decimalDigits;
  info.precision = clampSmall(static_cast<int64_t>(columnSize));
  info.numPrecRadix = radix;
  info.defaultCType = defaultC;
  info.unsignedAttr = false;
  info.searchable = SQL_PRED_BASIC;
}

void setDatetime(OdbcTypeInfo& info, SQLSMALLINT conciseType, SQLSMALLINT subcode, int64_t columnSize,
                 SQLLEN octetLength, int fractionDigits, SQLSMALLINT defaultC) {
  info.conciseType = conciseType;
  info.verboseType = SQL_DATETIME;
  info.datetimeSubcode = subcode;
  info.columnSize = clampULen(columnSize);
  info.octetLength = octetLength;
  info.displaySize = clampLen(static_cast<uint64_t>(columnSize));
  info.decimalDigits = static_cast<SQLSMALLINT>(fractionDigits);
  info.precision = static_cast<SQLSMALLINT>(fractionDigits);
  info.numPrecRadix = 0;
  info.defaultCType = defaultC;
  info.searchable = SQL_PRED_BASIC;
}

/// Length of Trino's textual form "HH:MM:SS[.fff]" / "YYYY-MM-DD HH:MM:SS[.fff]" for precision p.
int64_t temporalTextLength(int64_t base, int p) { return base + (p > 0 ? p + 1 : 0); }

/// Text exposure for types that only have a string representation in this mode (time/timestamp
/// with time zone in String mode, intervals in String mode, uuid, ipaddress). They are not
/// searchable with LIKE on the server, so they are reported SQL_PRED_BASIC.
void setTextual(OdbcTypeInfo& info, const TypeMappingOptions& opts, int64_t chars) {
  setCharacter(info, opts, SQL_VARCHAR, SQL_WVARCHAR, chars);
  info.caseSensitive = false;
  info.searchable = SQL_PRED_BASIC;
}

/// The OdbcTypeInfo of a Trino type as one SQLGetTypeInfo row: TYPE_NAME becomes the base name
/// and CREATE_PARAMS/scale bounds are taken from the mapping.
OdbcTypeInfo typeInfoRow(std::string_view trinoType, const TypeMappingOptions& opts, const char* typeName) {
  OdbcTypeInfo info = mapTrinoType(TrinoType::parse(trinoType), opts);
  info.typeName = typeName;
  info.localTypeName = typeName;
  return info;
}

}  // namespace

StorageKind storageKindFor(TypeCategory category) {
  switch (category) {
    case TypeCategory::Boolean:
      return StorageKind::Bool;
    case TypeCategory::TinyInt:
    case TypeCategory::SmallInt:
    case TypeCategory::Integer:
    case TypeCategory::BigInt:
      return StorageKind::Int64;
    case TypeCategory::Real:
    case TypeCategory::Double:
      return StorageKind::Double;
    case TypeCategory::Varbinary:
      return StorageKind::Binary;
    default:
      return StorageKind::String;
  }
}

OdbcTypeInfo mapTrinoType(const TrinoType& type, const TypeMappingOptions& opts) {
  OdbcTypeInfo info;
  const int64_t L = std::max(opts.defaultStringLength, 1);

  info.category = type.category;
  info.storage = storageKindFor(type.category);
  info.typeName = type.toString();
  info.localTypeName = info.typeName;
  info.nullable = SQL_NULLABLE;
  info.fixedPrecScale = false;
  info.autoIncrement = false;
  info.unsignedAttr = !type.isNumeric();
  info.caseSensitive = false;
  info.searchable = SQL_PRED_BASIC;
  info.temporalPrecision = type.temporalPrecision();

  switch (type.category) {
    case TypeCategory::Boolean:
      info.conciseType = SQL_BIT;
      info.verboseType = SQL_BIT;
      info.columnSize = 1;
      info.octetLength = 1;
      info.displaySize = 1;
      info.decimalDigits = 0;
      info.precision = 1;
      info.numPrecRadix = 0;
      info.defaultCType = SQL_C_BIT;
      break;

    case TypeCategory::TinyInt:
      setNumeric(info, SQL_TINYINT, 3, 1, 4, 0, 10, SQL_C_STINYINT);
      break;
    case TypeCategory::SmallInt:
      setNumeric(info, SQL_SMALLINT, 5, 2, 6, 0, 10, SQL_C_SSHORT);
      break;
    case TypeCategory::Integer:
      setNumeric(info, SQL_INTEGER, 10, 4, 11, 0, 10, SQL_C_SLONG);
      break;
    case TypeCategory::BigInt:
      setNumeric(info, SQL_BIGINT, 19, 8, 20, 0, 10, SQL_C_SBIGINT);
      break;
    // Approximate numerics report their precision in decimal digits (7 / 15, the Appendix D column
    // sizes), so NUM_PREC_RADIX must be 10: radix 2 would make clients read 7 / 15 as bit counts.
    case TypeCategory::Real:
      setNumeric(info, SQL_REAL, 7, 4, 14, 0, 10, SQL_C_FLOAT);
      break;
    case TypeCategory::Double:
      setNumeric(info, SQL_DOUBLE, 15, 8, 24, 0, 10, SQL_C_DOUBLE);
      break;

    case TypeCategory::Decimal: {
      const int p = type.decimalPrecision();
      const int s = type.decimalScale();
      if (opts.decimalAsDouble) {
        // Tools without SQL_NUMERIC support see a plain double; the value itself stays exact text.
        setNumeric(info, SQL_DOUBLE, 15, 8, 24, 0, 10, SQL_C_DOUBLE);
      } else {
        // Display: digits + sign + decimal point.
        setNumeric(info, SQL_DECIMAL, static_cast<SQLULEN>(p), p + 2, p + 2, static_cast<SQLSMALLINT>(s), 10,
                   SQL_C_CHAR);
      }
      info.createParams = "precision,scale";
      info.minScale = 0;
      info.maxScale = 38;
      break;
    }

    case TypeCategory::Varchar: {
      const std::optional<int64_t> n = type.length();
      if (n.has_value()) {
        setCharacter(info, opts, SQL_VARCHAR, SQL_WVARCHAR, *n);
      } else if (opts.unboundedAsLong) {
        setCharacter(info, opts, SQL_LONGVARCHAR, SQL_WLONGVARCHAR, L);
      } else {
        setCharacter(info, opts, SQL_VARCHAR, SQL_WVARCHAR, L);
      }
      info.caseSensitive = true;
      info.searchable = SQL_SEARCHABLE;
      info.literalPrefix = "'";
      info.literalSuffix = "'";
      info.createParams = "length";
      break;
    }

    case TypeCategory::Char:
      setCharacter(info, opts, SQL_CHAR, SQL_WCHAR, type.length().value_or(1));
      info.caseSensitive = true;
      info.searchable = SQL_SEARCHABLE;
      info.literalPrefix = "'";
      info.literalSuffix = "'";
      info.createParams = "length";
      break;

    case TypeCategory::Varbinary:
      info.conciseType = opts.unboundedAsLong ? SQL_LONGVARBINARY : SQL_VARBINARY;
      info.verboseType = info.conciseType;
      info.columnSize = clampULen(L);
      info.octetLength = clampLen(static_cast<uint64_t>(L));
      info.displaySize = clampLen(static_cast<uint64_t>(L) * 2);  // hex digits
      info.decimalDigits = 0;
      info.precision = clampSmall(L);
      info.numPrecRadix = 0;
      info.defaultCType = SQL_C_BINARY;
      info.literalPrefix = "X'";
      info.literalSuffix = "'";
      break;

    case TypeCategory::Json:
      setCharacter(info, opts, SQL_LONGVARCHAR, SQL_WLONGVARCHAR, L);
      info.caseSensitive = true;
      info.searchable = SQL_SEARCHABLE;
      info.literalPrefix = "JSON '";
      info.literalSuffix = "'";
      break;

    case TypeCategory::Date:
      setDatetime(info, SQL_TYPE_DATE, SQL_CODE_DATE, 10, sizeof(SQL_DATE_STRUCT), 0, SQL_C_TYPE_DATE);
      info.literalPrefix = "DATE '";
      info.literalSuffix = "'";
      break;

    case TypeCategory::Time: {
      const int p = type.temporalPrecision();
      setDatetime(info, SQL_TYPE_TIME, SQL_CODE_TIME, temporalTextLength(8, p), sizeof(SQL_TIME_STRUCT),
                  std::min(p, 9), SQL_C_TYPE_TIME);
      info.literalPrefix = "TIME '";
      info.literalSuffix = "'";
      info.createParams = "precision";
      info.minScale = 0;
      info.maxScale = 9;
      break;
    }

    case TypeCategory::TimeWithTimeZone: {
      const int p = type.temporalPrecision();
      if (opts.timeTz == TimeTzMode::String) {
        // "HH:MM:SS[.fff]+HH:MM"
        setTextual(info, opts, temporalTextLength(8, p) + 6);
      } else {
        setDatetime(info, SQL_TYPE_TIME, SQL_CODE_TIME, temporalTextLength(8, p), sizeof(SQL_TIME_STRUCT),
                    std::min(p, 9), SQL_C_TYPE_TIME);
        info.createParams = "precision";
        info.minScale = 0;
        info.maxScale = 9;
      }
      info.literalPrefix = "TIME '";
      info.literalSuffix = "'";
      break;
    }

    case TypeCategory::Timestamp: {
      const int p = type.temporalPrecision();
      setDatetime(info, SQL_TYPE_TIMESTAMP, SQL_CODE_TIMESTAMP, temporalTextLength(19, p),
                  sizeof(SQL_TIMESTAMP_STRUCT), std::min(p, 9), SQL_C_TYPE_TIMESTAMP);
      info.literalPrefix = "TIMESTAMP '";
      info.literalSuffix = "'";
      info.createParams = "precision";
      info.minScale = 0;
      info.maxScale = 9;
      break;
    }

    case TypeCategory::TimestampWithTimeZone: {
      const int p = type.temporalPrecision();
      if (opts.timestampTz == TimestampTzMode::String) {
        // "YYYY-MM-DD HH:MM:SS[.fff] <zone>": IANA names fit in 40 characters.
        setTextual(info, opts, temporalTextLength(19, p) + 1 + 40);
      } else {
        setDatetime(info, SQL_TYPE_TIMESTAMP, SQL_CODE_TIMESTAMP, temporalTextLength(19, p),
                    sizeof(SQL_TIMESTAMP_STRUCT), std::min(p, 9), SQL_C_TYPE_TIMESTAMP);
        info.createParams = "precision";
        info.minScale = 0;
        info.maxScale = 9;
      }
      info.literalPrefix = "TIMESTAMP '";
      info.literalSuffix = "'";
      break;
    }

    case TypeCategory::IntervalYearToMonth:
      // In String mode the column is plain text: SQL_DATETIME_SUB / SQL_DESC_DATETIME_INTERVAL_CODE
      // must then be NULL / 0, so the interval flags are only set when the column is an SQL_INTERVAL.
      if (opts.interval == IntervalMode::String) {
        setTextual(info, opts, 20);
      } else {
        info.intervalType = true;
        info.intervalCode = SQL_CODE_YEAR_TO_MONTH;
        info.conciseType = SQL_INTERVAL_YEAR_TO_MONTH;
        info.verboseType = SQL_INTERVAL;
        info.datetimeSubcode = SQL_CODE_YEAR_TO_MONTH;
        info.columnSize = 20;
        info.octetLength = sizeof(SQL_INTERVAL_STRUCT);
        info.displaySize = 20;
        info.decimalDigits = 0;
        info.precision = 0;
        info.numPrecRadix = 0;
        info.defaultCType = SQL_C_INTERVAL_YEAR_TO_MONTH;
      }
      info.literalPrefix = "INTERVAL '";
      info.literalSuffix = "' YEAR TO MONTH";
      break;

    case TypeCategory::IntervalDayToSecond:
      if (opts.interval == IntervalMode::String) {
        setTextual(info, opts, 30);
      } else {
        info.intervalType = true;
        info.intervalCode = SQL_CODE_DAY_TO_SECOND;
        info.conciseType = SQL_INTERVAL_DAY_TO_SECOND;
        info.verboseType = SQL_INTERVAL;
        info.datetimeSubcode = SQL_CODE_DAY_TO_SECOND;
        info.columnSize = 30;
        info.octetLength = sizeof(SQL_INTERVAL_STRUCT);
        info.displaySize = 30;
        info.decimalDigits = 3;  // Trino intervals carry milliseconds
        info.precision = 3;
        info.numPrecRadix = 0;
        info.defaultCType = SQL_C_INTERVAL_DAY_TO_SECOND;
      }
      info.literalPrefix = "INTERVAL '";
      info.literalSuffix = "' DAY TO SECOND";
      break;

    case TypeCategory::Array:
    case TypeCategory::Map:
    case TypeCategory::Row:
      // Exposed as their JSON text; there is no literal syntax an application could use.
      setCharacter(info, opts, SQL_LONGVARCHAR, SQL_WLONGVARCHAR, L);
      info.caseSensitive = true;
      info.searchable = SQL_SEARCHABLE;
      break;

    case TypeCategory::Uuid:
      if (opts.uuidAsGuid) {
        info.conciseType = SQL_GUID;
        info.verboseType = SQL_GUID;
        info.columnSize = 36;
        info.octetLength = sizeof(SQLGUID);
        info.displaySize = 36;
        info.decimalDigits = 0;
        info.precision = 36;
        info.numPrecRadix = 0;
        info.defaultCType = SQL_C_GUID;
      } else {
        setTextual(info, opts, 36);
      }
      info.literalPrefix = "UUID '";
      info.literalSuffix = "'";
      break;

    case TypeCategory::IpAddress:
      setTextual(info, opts, 45);  // longest IPv6 text incl. embedded IPv4
      info.literalPrefix = "IPADDRESS '";
      info.literalSuffix = "'";
      break;

    case TypeCategory::Unknown:
      // HyperLogLog, Geometry, ... : the server always has a textual cast, so expose as text.
      setCharacter(info, opts, SQL_VARCHAR, SQL_WVARCHAR, L);
      info.caseSensitive = true;
      info.searchable = SQL_SEARCHABLE;
      break;
  }

  return info;
}

std::vector<OdbcTypeInfo> odbcTypeInfoTable(const TypeMappingOptions& opts) {
  std::vector<OdbcTypeInfo> rows;
  rows.reserve(24);
  const int64_t L = std::max(opts.defaultStringLength, 1);
  const std::string varcharL = "varchar(" + std::to_string(L) + ")";
  // Trino caps char length at 65536.
  const std::string charL = "char(" + std::to_string(std::min<int64_t>(L, 65536)) + ")";

  if (opts.uuidAsGuid) rows.push_back(typeInfoRow("uuid", opts, "uuid"));

  // (W)LONGVARCHAR: the unbounded varchar in every mode, so an application that asks for a long
  // text type gets a plain text type for CREATE TABLE / CAST (json would turn 'abc' into the JSON
  // string "\"abc\"" and reject non-JSON text). json stays discoverable right after it.
  {
    TypeMappingOptions longOpts = opts;
    longOpts.unboundedAsLong = true;
    OdbcTypeInfo longVarchar = typeInfoRow("varchar", longOpts, "varchar");
    longVarchar.createParams.clear();  // unbounded: no length parameter
    rows.push_back(std::move(longVarchar));
  }
  rows.push_back(typeInfoRow("json", opts, "json"));

  {
    // The (W)VARCHAR row must not turn into a long type, whatever UnboundedVarcharAsLongVarchar says.
    OdbcTypeInfo varchar = typeInfoRow(varcharL, opts, "varchar");
    rows.push_back(std::move(varchar));
  }
  rows.push_back(typeInfoRow(charL, opts, "char"));
  rows.push_back(typeInfoRow("boolean", opts, "boolean"));
  rows.push_back(typeInfoRow("tinyint", opts, "tinyint"));
  rows.push_back(typeInfoRow("bigint", opts, "bigint"));
  rows.push_back(typeInfoRow("varbinary", opts, "varbinary"));
  rows.push_back(typeInfoRow("double", opts, "double"));
  rows.push_back(typeInfoRow("decimal", opts, "decimal"));  // decimal(38,0): the widest form
  rows.push_back(typeInfoRow("integer", opts, "integer"));
  rows.push_back(typeInfoRow("smallint", opts, "smallint"));
  rows.push_back(typeInfoRow("real", opts, "real"));
  rows.push_back(typeInfoRow("date", opts, "date"));
  // ODBC structs carry at most nanoseconds, so the catalog row advertises precision 9.
  rows.push_back(typeInfoRow("time(9)", opts, "time"));
  rows.push_back(typeInfoRow("timestamp(9)", opts, "timestamp"));
  if (opts.interval == IntervalMode::Interval) {
    rows.push_back(typeInfoRow("interval year to month", opts, "interval year to month"));
    rows.push_back(typeInfoRow("interval day to second", opts, "interval day to second"));
  }

  // The spec orders by DATA_TYPE ascending with the closest match first inside a DATA_TYPE; the
  // insertion order above already lists closer matches first (double before decimal-as-double).
  std::stable_sort(rows.begin(), rows.end(),
                   [](const OdbcTypeInfo& a, const OdbcTypeInfo& b) { return a.conciseType < b.conciseType; });
  TRINO_LOG_TRACE(kLog, "type info table built with %zu rows", rows.size());
  return rows;
}

SQLSMALLINT defaultCTypeFor(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return SQL_C_CHAR;
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      return SQL_C_WCHAR;
    case SQL_BIT:
      return SQL_C_BIT;
    case SQL_TINYINT:
      return SQL_C_STINYINT;
    case SQL_SMALLINT:
      return SQL_C_SSHORT;
    case SQL_INTEGER:
      return SQL_C_SLONG;
    case SQL_BIGINT:
      return SQL_C_SBIGINT;
    case SQL_REAL:
      return SQL_C_FLOAT;
    case SQL_FLOAT:
    case SQL_DOUBLE:
      return SQL_C_DOUBLE;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return SQL_C_BINARY;
    case SQL_TYPE_DATE:
    case SQL_DATE:  // == SQL_DATETIME (verbose)
      return SQL_C_TYPE_DATE;
    case SQL_TYPE_TIME:
    case SQL_TIME:  // == SQL_INTERVAL (verbose)
      return SQL_C_TYPE_TIME;
    case SQL_TYPE_TIMESTAMP:
    case SQL_TIMESTAMP:
      return SQL_C_TYPE_TIMESTAMP;
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      return sqlType;  // SQL_C_INTERVAL_* share the SQL_INTERVAL_* values
    case SQL_GUID:
      return SQL_C_GUID;
    default:
      return SQL_C_CHAR;
  }
}

SQLSMALLINT verboseTypeOf(SQLSMALLINT conciseType) {
  switch (conciseType) {
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
    case SQL_TIMESTAMP:  // legacy 11
      return SQL_DATETIME;
    default:
      break;
  }
  if (conciseType >= SQL_INTERVAL_YEAR && conciseType <= SQL_INTERVAL_MINUTE_TO_SECOND) return SQL_INTERVAL;
  // SQL_DATE (9) and SQL_TIME (10) already are the verbose SQL_DATETIME / SQL_INTERVAL values.
  return conciseType;
}

SQLSMALLINT datetimeSubcodeOf(SQLSMALLINT conciseType) {
  switch (conciseType) {
    case SQL_TYPE_DATE:
      return SQL_CODE_DATE;
    case SQL_TYPE_TIME:
      return SQL_CODE_TIME;
    case SQL_TYPE_TIMESTAMP:
    case SQL_TIMESTAMP:
      return SQL_CODE_TIMESTAMP;
    default:
      break;
  }
  if (conciseType >= SQL_INTERVAL_YEAR && conciseType <= SQL_INTERVAL_MINUTE_TO_SECOND) {
    return static_cast<SQLSMALLINT>(conciseType - 100);  // SQL_CODE_YEAR .. SQL_CODE_MINUTE_TO_SECOND
  }
  return 0;
}

SQLSMALLINT conciseTypeOf(SQLSMALLINT verboseType, SQLSMALLINT subcode) {
  if (verboseType == SQL_DATETIME) {
    switch (subcode) {
      case SQL_CODE_DATE:
        return SQL_TYPE_DATE;
      case SQL_CODE_TIME:
        return SQL_TYPE_TIME;
      case SQL_CODE_TIMESTAMP:
        return SQL_TYPE_TIMESTAMP;
      default:
        return verboseType;
    }
  }
  if (verboseType == SQL_INTERVAL && subcode >= SQL_CODE_YEAR && subcode <= SQL_CODE_MINUTE_TO_SECOND) {
    return static_cast<SQLSMALLINT>(100 + subcode);
  }
  return verboseType;
}

bool isValidSqlType(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_DECIMAL:
    case SQL_NUMERIC:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
    case SQL_BIT:
    case SQL_TINYINT:
    case SQL_BIGINT:
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
    case SQL_DATE:       // legacy / SQL_DATETIME
    case SQL_TIME:       // legacy / SQL_INTERVAL
    case SQL_TIMESTAMP:  // legacy
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
    case SQL_GUID:
      return true;
    default:
      return false;
  }
}

bool isValidCType(SQLSMALLINT cType) {
  switch (cType) {
    case SQL_C_CHAR:
    case SQL_C_WCHAR:
    case SQL_C_SSHORT:
    case SQL_C_USHORT:
    case SQL_C_SHORT:  // ODBC 2.x
    case SQL_C_SLONG:
    case SQL_C_ULONG:
    case SQL_C_LONG:   // ODBC 2.x
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE:
    case SQL_C_BIT:
    case SQL_C_STINYINT:
    case SQL_C_UTINYINT:
    case SQL_C_TINYINT:  // ODBC 2.x
    case SQL_C_SBIGINT:
    case SQL_C_UBIGINT:
    case SQL_C_BINARY:   // also SQL_C_VARBOOKMARK
    case SQL_C_TYPE_DATE:
    case SQL_C_TYPE_TIME:
    case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_DATE:       // legacy
    case SQL_C_TIME:       // legacy
    case SQL_C_TIMESTAMP:  // legacy
    case SQL_C_NUMERIC:
    case SQL_C_GUID:
    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_YEAR_TO_MONTH:
    case SQL_C_INTERVAL_DAY_TO_HOUR:
    case SQL_C_INTERVAL_DAY_TO_MINUTE:
    case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    case SQL_C_INTERVAL_MINUTE_TO_SECOND:
    case SQL_C_DEFAULT:
      return true;
    default:
      return false;
  }
}

std::string trinoTypeForSqlType(SQLSMALLINT sqlType, SQLULEN columnSize, SQLSMALLINT decimalDigits) {
  switch (sqlType) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      if (columnSize > 0 && columnSize < 2147483647ULL) return "varchar(" + std::to_string(columnSize) + ")";
      return "varchar";
    case SQL_BIT:
      return "boolean";
    case SQL_TINYINT:
      return "tinyint";
    case SQL_SMALLINT:
      return "smallint";
    case SQL_INTEGER:
      return "integer";
    case SQL_BIGINT:
      return "bigint";
    case SQL_REAL:
      return "real";
    case SQL_FLOAT:
    case SQL_DOUBLE:
      return "double";
    case SQL_DECIMAL:
    case SQL_NUMERIC: {
      const int p = columnSize == 0 ? 38 : static_cast<int>(std::min<SQLULEN>(columnSize, 38));
      const int s = std::min(std::max(static_cast<int>(decimalDigits), 0), p);
      return "decimal(" + std::to_string(p) + "," + std::to_string(s) + ")";
    }
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return "varbinary";
    case SQL_TYPE_DATE:
    case SQL_DATE:
      return "date";
    case SQL_TYPE_TIME:
    case SQL_TIME: {
      const int p = std::min(std::max(static_cast<int>(decimalDigits), 0), 12);
      return "time(" + std::to_string(p) + ")";
    }
    case SQL_TYPE_TIMESTAMP:
    case SQL_TIMESTAMP: {
      // Applications routinely bind timestamps with DecimalDigits 0; Trino's default precision keeps
      // milliseconds instead of silently truncating them.
      const int p = decimalDigits > 0 ? std::min(static_cast<int>(decimalDigits), 12) : 3;
      return "timestamp(" + std::to_string(p) + ")";
    }
    case SQL_GUID:
      return "uuid";
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_YEAR_TO_MONTH:
      return "interval year to month";
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      return "interval day to second";
    default:
      return "varchar";
  }
}

const char* sqlTypeName(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_UNKNOWN_TYPE: return "SQL_UNKNOWN_TYPE";
    case SQL_CHAR: return "SQL_CHAR";
    case SQL_NUMERIC: return "SQL_NUMERIC";
    case SQL_DECIMAL: return "SQL_DECIMAL";
    case SQL_INTEGER: return "SQL_INTEGER";
    case SQL_SMALLINT: return "SQL_SMALLINT";
    case SQL_FLOAT: return "SQL_FLOAT";
    case SQL_REAL: return "SQL_REAL";
    case SQL_DOUBLE: return "SQL_DOUBLE";
    case SQL_DATETIME: return "SQL_DATETIME";      // == SQL_DATE
    case SQL_INTERVAL: return "SQL_INTERVAL";      // == SQL_TIME
    case SQL_TIMESTAMP: return "SQL_TIMESTAMP";
    case SQL_VARCHAR: return "SQL_VARCHAR";
    case SQL_TYPE_DATE: return "SQL_TYPE_DATE";
    case SQL_TYPE_TIME: return "SQL_TYPE_TIME";
    case SQL_TYPE_TIMESTAMP: return "SQL_TYPE_TIMESTAMP";
    case SQL_LONGVARCHAR: return "SQL_LONGVARCHAR";
    case SQL_BINARY: return "SQL_BINARY";
    case SQL_VARBINARY: return "SQL_VARBINARY";
    case SQL_LONGVARBINARY: return "SQL_LONGVARBINARY";
    case SQL_BIGINT: return "SQL_BIGINT";
    case SQL_TINYINT: return "SQL_TINYINT";
    case SQL_BIT: return "SQL_BIT";
    case SQL_WCHAR: return "SQL_WCHAR";
    case SQL_WVARCHAR: return "SQL_WVARCHAR";
    case SQL_WLONGVARCHAR: return "SQL_WLONGVARCHAR";
    case SQL_GUID: return "SQL_GUID";
    case SQL_INTERVAL_YEAR: return "SQL_INTERVAL_YEAR";
    case SQL_INTERVAL_MONTH: return "SQL_INTERVAL_MONTH";
    case SQL_INTERVAL_DAY: return "SQL_INTERVAL_DAY";
    case SQL_INTERVAL_HOUR: return "SQL_INTERVAL_HOUR";
    case SQL_INTERVAL_MINUTE: return "SQL_INTERVAL_MINUTE";
    case SQL_INTERVAL_SECOND: return "SQL_INTERVAL_SECOND";
    case SQL_INTERVAL_YEAR_TO_MONTH: return "SQL_INTERVAL_YEAR_TO_MONTH";
    case SQL_INTERVAL_DAY_TO_HOUR: return "SQL_INTERVAL_DAY_TO_HOUR";
    case SQL_INTERVAL_DAY_TO_MINUTE: return "SQL_INTERVAL_DAY_TO_MINUTE";
    case SQL_INTERVAL_DAY_TO_SECOND: return "SQL_INTERVAL_DAY_TO_SECOND";
    case SQL_INTERVAL_HOUR_TO_MINUTE: return "SQL_INTERVAL_HOUR_TO_MINUTE";
    case SQL_INTERVAL_HOUR_TO_SECOND: return "SQL_INTERVAL_HOUR_TO_SECOND";
    case SQL_INTERVAL_MINUTE_TO_SECOND: return "SQL_INTERVAL_MINUTE_TO_SECOND";
    default: return "SQL_<unknown>";
  }
}

const char* cTypeName(SQLSMALLINT cType) {
  switch (cType) {
    case SQL_C_CHAR: return "SQL_C_CHAR";
    case SQL_C_WCHAR: return "SQL_C_WCHAR";
    case SQL_C_SSHORT: return "SQL_C_SSHORT";
    case SQL_C_USHORT: return "SQL_C_USHORT";
    case SQL_C_SHORT: return "SQL_C_SHORT";
    case SQL_C_SLONG: return "SQL_C_SLONG";
    case SQL_C_ULONG: return "SQL_C_ULONG";
    case SQL_C_LONG: return "SQL_C_LONG";
    case SQL_C_FLOAT: return "SQL_C_FLOAT";
    case SQL_C_DOUBLE: return "SQL_C_DOUBLE";
    case SQL_C_BIT: return "SQL_C_BIT";
    case SQL_C_STINYINT: return "SQL_C_STINYINT";
    case SQL_C_UTINYINT: return "SQL_C_UTINYINT";
    case SQL_C_TINYINT: return "SQL_C_TINYINT";
    case SQL_C_SBIGINT: return "SQL_C_SBIGINT";
    case SQL_C_UBIGINT: return "SQL_C_UBIGINT";
    case SQL_C_BINARY: return "SQL_C_BINARY";
    case SQL_C_TYPE_DATE: return "SQL_C_TYPE_DATE";
    case SQL_C_TYPE_TIME: return "SQL_C_TYPE_TIME";
    case SQL_C_TYPE_TIMESTAMP: return "SQL_C_TYPE_TIMESTAMP";
    case SQL_C_DATE: return "SQL_C_DATE";
    case SQL_C_TIME: return "SQL_C_TIME";
    case SQL_C_TIMESTAMP: return "SQL_C_TIMESTAMP";
    case SQL_C_NUMERIC: return "SQL_C_NUMERIC";
    case SQL_C_GUID: return "SQL_C_GUID";
    case SQL_C_INTERVAL_YEAR: return "SQL_C_INTERVAL_YEAR";
    case SQL_C_INTERVAL_MONTH: return "SQL_C_INTERVAL_MONTH";
    case SQL_C_INTERVAL_DAY: return "SQL_C_INTERVAL_DAY";
    case SQL_C_INTERVAL_HOUR: return "SQL_C_INTERVAL_HOUR";
    case SQL_C_INTERVAL_MINUTE: return "SQL_C_INTERVAL_MINUTE";
    case SQL_C_INTERVAL_SECOND: return "SQL_C_INTERVAL_SECOND";
    case SQL_C_INTERVAL_YEAR_TO_MONTH: return "SQL_C_INTERVAL_YEAR_TO_MONTH";
    case SQL_C_INTERVAL_DAY_TO_HOUR: return "SQL_C_INTERVAL_DAY_TO_HOUR";
    case SQL_C_INTERVAL_DAY_TO_MINUTE: return "SQL_C_INTERVAL_DAY_TO_MINUTE";
    case SQL_C_INTERVAL_DAY_TO_SECOND: return "SQL_C_INTERVAL_DAY_TO_SECOND";
    case SQL_C_INTERVAL_HOUR_TO_MINUTE: return "SQL_C_INTERVAL_HOUR_TO_MINUTE";
    case SQL_C_INTERVAL_HOUR_TO_SECOND: return "SQL_C_INTERVAL_HOUR_TO_SECOND";
    case SQL_C_INTERVAL_MINUTE_TO_SECOND: return "SQL_C_INTERVAL_MINUTE_TO_SECOND";
    case SQL_C_DEFAULT: return "SQL_C_DEFAULT";
    default: return "SQL_C_<unknown>";
  }
}

bool isChunkedCType(SQLSMALLINT cType) {
  return cType == SQL_C_CHAR || cType == SQL_C_WCHAR || cType == SQL_C_BINARY;
}

SQLSMALLINT normalizeCType(SQLSMALLINT cType) {
  switch (cType) {
    case SQL_C_DATE:
      return SQL_C_TYPE_DATE;
    case SQL_C_TIME:
      return SQL_C_TYPE_TIME;
    case SQL_C_TIMESTAMP:
      return SQL_C_TYPE_TIMESTAMP;
    // ODBC 2.x integer identifiers are the signed ODBC 3 ones.
    case SQL_C_SHORT:
      return SQL_C_SSHORT;
    case SQL_C_LONG:
      return SQL_C_SLONG;
    case SQL_C_TINYINT:
      return SQL_C_STINYINT;
    default:
      return cType;
  }
}

}  // namespace trino_odbc
