// SQLGetTypeInfo: a static, in-memory result set built from the type-mapping table so that the
// types it advertises are exactly the ones SQLDescribeCol / SQLColumns report.
#include "catalog/catalog.h"

#include "result/page.h"
#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/log.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "catalog";
constexpr const char* kTypeNameType = "varchar(128)";
constexpr const char* kShortTextType = "varchar(32)";

// Cell helpers: std::variant's converting constructor is ambiguous for short / const char*, so
// every cell is built from an exact alternative type.
Cell nullCell() { return Cell{std::monostate{}}; }
Cell intCell(int64_t v) { return Cell{v}; }
Cell textCell(std::string s) { return Cell{std::move(s)}; }
Cell textOrNull(const std::string& s) { return s.empty() ? nullCell() : textCell(s); }
Cell boolCell(bool v) { return intCell(v ? SQL_TRUE : SQL_FALSE); }

/// Maps the DataType argument onto the DATA_TYPE value stored in the table: ODBC 2.x identifiers
/// (SQL_DATE / SQL_TIME / SQL_TIMESTAMP) select the ODBC 3 rows, and the synonyms an application may
/// ask for (SQL_NUMERIC, SQL_FLOAT) select the row of the type Trino really has.
SQLSMALLINT requestedDataType(SQLSMALLINT dataType) {
  switch (dataType) {
    case SQL_DATE:
      return SQL_TYPE_DATE;
    case SQL_TIME:
      return SQL_TYPE_TIME;
    case SQL_TIMESTAMP:
      return SQL_TYPE_TIMESTAMP;
    case SQL_NUMERIC:
      return SQL_DECIMAL;
    case SQL_FLOAT:
      return SQL_DOUBLE;
    default:
      return dataType;
  }
}

/// DATA_TYPE as an ODBC 2.x application knows it: the 2.x date/time identifiers (9/10/11).
SQLSMALLINT odbc2DataType(SQLSMALLINT conciseType) {
  switch (conciseType) {
    case SQL_TYPE_DATE:
      return SQL_DATE;
    case SQL_TYPE_TIME:
      return SQL_TIME;
    case SQL_TYPE_TIMESTAMP:
      return SQL_TIMESTAMP;
    default:
      return conciseType;
  }
}

bool hasScaleBounds(const OdbcTypeInfo& t) {
  switch (t.category) {
    case TypeCategory::Decimal:
    case TypeCategory::Time:
    case TypeCategory::Timestamp:
    case TypeCategory::TimeWithTimeZone:
    case TypeCategory::TimestampWithTimeZone:
      return true;
    default:
      return false;
  }
}

/// INTERVAL_PRECISION (leading field precision) as the spec fixes it: 2 for year-month, 9 for
/// day-second, NULL otherwise.
Cell intervalPrecision(const OdbcTypeInfo& t) {
  if (t.category == TypeCategory::IntervalYearToMonth) return intCell(2);
  if (t.category == TypeCategory::IntervalDayToSecond) return intCell(9);
  return nullCell();
}

}  // namespace

std::unique_ptr<MemoryRowSource> catalogTypeInfo(Connection& conn, SQLSMALLINT dataType) {
  if (dataType != SQL_ALL_TYPES && !isValidSqlType(dataType)) {
    throw DriverError(sqlstate::kInvalidSqlDataType,
                      "SQLGetTypeInfo: invalid SQL data type " + std::to_string(dataType));
  }

  const TypeMappingOptions opts = conn.config.typeMapping();
  // An ODBC 2.x application expects the 2.x date/time identifiers in DATA_TYPE.
  const bool odbc2 = conn.env != nullptr && conn.env->odbcVersion == SQL_OV_ODBC2;

  // Explicit lengths keep the text columns (W)VARCHAR even when UnboundedVarcharAsLongVarchar=1
  // (Power BI reads long types through its blob paths); the spec types TYPE_NAME as Varchar(128).
  std::vector<ResultColumn> columns;
  columns.reserve(19);
  columns.push_back(ResultColumn::make("TYPE_NAME", kTypeNameType, opts));
  columns.push_back(ResultColumn::make("DATA_TYPE", "smallint", opts));
  columns.push_back(ResultColumn::make("COLUMN_SIZE", "integer", opts));
  columns.push_back(ResultColumn::make("LITERAL_PREFIX", kShortTextType, opts));
  columns.push_back(ResultColumn::make("LITERAL_SUFFIX", kShortTextType, opts));
  columns.push_back(ResultColumn::make("CREATE_PARAMS", kShortTextType, opts));
  columns.push_back(ResultColumn::make("NULLABLE", "smallint", opts));
  columns.push_back(ResultColumn::make("CASE_SENSITIVE", "smallint", opts));
  columns.push_back(ResultColumn::make("SEARCHABLE", "smallint", opts));
  columns.push_back(ResultColumn::make("UNSIGNED_ATTRIBUTE", "smallint", opts));
  columns.push_back(ResultColumn::make("FIXED_PREC_SCALE", "smallint", opts));
  columns.push_back(ResultColumn::make("AUTO_UNIQUE_VALUE", "smallint", opts));
  columns.push_back(ResultColumn::make("LOCAL_TYPE_NAME", kTypeNameType, opts));
  columns.push_back(ResultColumn::make("MINIMUM_SCALE", "smallint", opts));
  columns.push_back(ResultColumn::make("MAXIMUM_SCALE", "smallint", opts));
  columns.push_back(ResultColumn::make("SQL_DATA_TYPE", "smallint", opts));
  columns.push_back(ResultColumn::make("SQL_DATETIME_SUB", "smallint", opts));
  columns.push_back(ResultColumn::make("NUM_PREC_RADIX", "integer", opts));
  columns.push_back(ResultColumn::make("INTERVAL_PRECISION", "smallint", opts));
  // The columns the spec declares NOT NULL.
  for (size_t i : {size_t{0}, size_t{1}, size_t{6}, size_t{8}, size_t{15}}) columns[i].odbc.nullable = SQL_NO_NULLS;

  auto source = std::make_unique<MemoryRowSource>(std::move(columns));
  PageBuilder builder(source->storageKinds());

  const SQLSMALLINT wanted = requestedDataType(dataType);
  size_t emitted = 0;
  for (const OdbcTypeInfo& t : odbcTypeInfoTable(opts)) {
    if (dataType != SQL_ALL_TYPES && t.conciseType != wanted) continue;
    // COLUMN_SIZE is a 32-bit column; the table never exceeds it but clamp rather than wrap.
    const int64_t columnSize = static_cast<int64_t>(std::min<SQLULEN>(t.columnSize, 2147483647ULL));
    builder.addRow({
        textCell(t.typeName),                                                        // TYPE_NAME
        intCell(odbc2 ? odbc2DataType(t.conciseType) : t.conciseType),               // DATA_TYPE
        intCell(columnSize),                                                         // COLUMN_SIZE
        textOrNull(t.literalPrefix),                                                 // LITERAL_PREFIX
        textOrNull(t.literalSuffix),                                                 // LITERAL_SUFFIX
        textOrNull(t.createParams),                                                  // CREATE_PARAMS
        intCell(t.nullable),                                                         // NULLABLE
        boolCell(t.caseSensitive),                                                   // CASE_SENSITIVE
        intCell(t.searchable),                                                       // SEARCHABLE
        boolCell(t.unsignedAttr),                                                    // UNSIGNED_ATTRIBUTE
        boolCell(t.fixedPrecScale),                                                  // FIXED_PREC_SCALE
        boolCell(t.autoIncrement),                                                   // AUTO_UNIQUE_VALUE
        textOrNull(t.localTypeName.empty() ? t.typeName : t.localTypeName),          // LOCAL_TYPE_NAME
        hasScaleBounds(t) ? intCell(t.minScale) : nullCell(),                        // MINIMUM_SCALE
        hasScaleBounds(t) ? intCell(t.maxScale) : nullCell(),                        // MAXIMUM_SCALE
        intCell(t.verboseType),                                                      // SQL_DATA_TYPE
        t.datetimeSubcode != 0 ? intCell(t.datetimeSubcode) : nullCell(),            // SQL_DATETIME_SUB
        t.numPrecRadix != 0 ? intCell(t.numPrecRadix) : nullCell(),                  // NUM_PREC_RADIX
        intervalPrecision(t),                                                        // INTERVAL_PRECISION
    });
    ++emitted;
  }

  // An empty result set is expressed by having no pages at all rather than one page of zero rows.
  if (emitted > 0) source->addPage(builder.finish());
  TRINO_LOG_DEBUG(kLog, "SQLGetTypeInfo(%s) -> %zu rows",
                  dataType == SQL_ALL_TYPES ? "SQL_ALL_TYPES" : sqlTypeName(dataType), emitted);
  return source;
}

}  // namespace trino_odbc
