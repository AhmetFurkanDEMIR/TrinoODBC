// Catalog functions: SQLTables, SQLColumns, SQLProcedures, SQLProcedureColumns run queries against
// Trino's `system.jdbc.*` tables (the same tables the Trino JDBC driver uses) and reshape the rows
// into the ODBC result-set layouts; SQLPrimaryKeys, SQLForeignKeys, SQLStatistics,
// SQLSpecialColumns, SQLTablePrivileges and SQLColumnPrivileges return empty result sets with the
// spec-mandated columns because Trino exposes no keys, indexes or grants through metadata.
// SQLGetTypeInfo (catalogTypeInfo) lives in type_info.cpp.
//
// Every type-dependent column of SQLColumns / SQLProcedureColumns is recomputed from the Trino
// type name with mapTrinoType() so that what SQLColumns reports and what SQLDescribeCol reports
// for the same column can never disagree, whatever the server put in its JDBC-flavoured columns.
#include "catalog/catalog.h"
#include "odbc/stmt_results.h"
#include "catalog/catalog_sql.h"

#include "core/client_cancel.h"

#include "result/page.h"
#include "types/odbc_types.h"
#include "types/trino_type.h"
#include "util/errors.h"
#include "util/log.h"
#include "util/strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trino_odbc {

// ---- MetadataCancelScope (core/client_cancel.h) ----------------------------------------------------

namespace {
/// Catalog calls run synchronously on the calling thread, so a thread-local is exactly as wide as
/// the ODBC call that installed it.
thread_local const std::atomic<bool>* tlsMetadataCancelFlag = nullptr;
}  // namespace

MetadataCancelScope::MetadataCancelScope(const std::atomic<bool>* flag) : previous_(tlsMetadataCancelFlag) {
  tlsMetadataCancelFlag = flag;
}

MetadataCancelScope::~MetadataCancelScope() { tlsMetadataCancelFlag = previous_; }

const std::atomic<bool>* MetadataCancelScope::current() { return tlsMetadataCancelFlag; }

namespace {

constexpr const char* kLog = "catalog";

// ---- SQL text helpers ----------------------------------------------------------------------------

/// Renders an ODBC search pattern as a Trino LIKE literal. `%`, `_` and the `\` escape have the
/// same meaning in both worlds, so the pattern is passed through; only quotes are doubled and a
/// backslash that escapes nothing is doubled, because Trino rejects a dangling escape character
/// ("Escape character must be followed by '%', '_' or the escape character itself") and an
/// application that forgot to escape a literal backslash should get no rows, not an error.
std::string likeLiteral(std::string_view pattern) {
  std::string out;
  out.reserve(pattern.size() + 2);
  out.push_back('\'');
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (c == '\'') {
      out.append("''");
      continue;
    }
    if (c == '\\') {
      const bool escapesSomething =
          i + 1 < pattern.size() && (pattern[i + 1] == '%' || pattern[i + 1] == '_' || pattern[i + 1] == '\\');
      if (escapesSomething) {
        out.push_back('\\');
        out.push_back(pattern[i + 1]);
        ++i;
      } else {
        out.append("\\\\");
      }
      continue;
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

/// Splits a value-list item into words separated by single spaces ("system   table" -> "SYSTEM TABLE").
std::string canonicalTableType(std::string_view item) {
  std::string out;
  out.reserve(item.size());
  bool pendingSpace = false;
  for (const char c : item) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(c);
  }
  return toUpper(out);
}

/// Strips one level of single quotes ('TABLE' -> TABLE) and undoes quote doubling inside.
std::string unquoteSingle(std::string_view item) {
  if (item.size() >= 2 && item.front() == '\'' && item.back() == '\'') {
    const std::string_view inner = item.substr(1, item.size() - 2);
    return replaceAll(inner, "''", "'");
  }
  return std::string(item);
}

bool isEmpty(const std::optional<std::string>& v) { return v.has_value() && v->empty(); }
bool isAll(const std::optional<std::string>& v) { return v.has_value() && *v == "%"; }

// ---- ODBC result layouts -------------------------------------------------------------------------

// Column widths of the catalog result sets. Explicit lengths keep the columns (W)VARCHAR even when
// UnboundedVarcharAsLongVarchar=1 (Power BI treats long types as blobs) while staying well above
// the identifier lengths Trino connectors produce; TYPE_NAME of a wide row(...) type may exceed
// the limit, in which case the fetch reports 01004 instead of silently dropping text.
constexpr const char* kIdentifierType = "varchar(256)";
constexpr const char* kTypeNameType = "varchar(4096)";
constexpr const char* kRemarksType = "varchar(4096)";
constexpr const char* kShortTextType = "varchar(32)";
constexpr const char* kFlagTextType = "varchar(3)";
constexpr const char* kSmallIntType = "smallint";
constexpr const char* kIntegerType = "integer";

struct ColumnSpec {
  const char* name;
  const char* trinoType;
  bool notNull;
};

const std::vector<ColumnSpec>& tablesLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false},  {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, false}, {"TABLE_TYPE", kShortTextType, false},
      {"REMARKS", kRemarksType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& columnsLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false},    {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, true},    {"COLUMN_NAME", kIdentifierType, true},
      {"DATA_TYPE", kSmallIntType, true},       {"TYPE_NAME", kTypeNameType, true},
      {"COLUMN_SIZE", kIntegerType, false},     {"BUFFER_LENGTH", kIntegerType, false},
      {"DECIMAL_DIGITS", kSmallIntType, false}, {"NUM_PREC_RADIX", kSmallIntType, false},
      {"NULLABLE", kSmallIntType, true},        {"REMARKS", kRemarksType, false},
      {"COLUMN_DEF", kRemarksType, false},      {"SQL_DATA_TYPE", kSmallIntType, true},
      {"SQL_DATETIME_SUB", kSmallIntType, false}, {"CHAR_OCTET_LENGTH", kIntegerType, false},
      {"ORDINAL_POSITION", kIntegerType, true}, {"IS_NULLABLE", kFlagTextType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& primaryKeysLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false}, {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, true}, {"COLUMN_NAME", kIdentifierType, true},
      {"KEY_SEQ", kSmallIntType, true},      {"PK_NAME", kIdentifierType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& foreignKeysLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"PKTABLE_CAT", kIdentifierType, false},  {"PKTABLE_SCHEM", kIdentifierType, false},
      {"PKTABLE_NAME", kIdentifierType, true},  {"PKCOLUMN_NAME", kIdentifierType, true},
      {"FKTABLE_CAT", kIdentifierType, false},  {"FKTABLE_SCHEM", kIdentifierType, false},
      {"FKTABLE_NAME", kIdentifierType, true},  {"FKCOLUMN_NAME", kIdentifierType, true},
      {"KEY_SEQ", kSmallIntType, true},         {"UPDATE_RULE", kSmallIntType, false},
      {"DELETE_RULE", kSmallIntType, false},    {"FK_NAME", kIdentifierType, false},
      {"PK_NAME", kIdentifierType, false},      {"DEFERRABILITY", kSmallIntType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& statisticsLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false},      {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, true},      {"NON_UNIQUE", kSmallIntType, false},
      {"INDEX_QUALIFIER", kIdentifierType, false}, {"INDEX_NAME", kIdentifierType, false},
      {"TYPE", kSmallIntType, true},              {"ORDINAL_POSITION", kSmallIntType, false},
      {"COLUMN_NAME", kIdentifierType, false},    {"ASC_OR_DESC", "varchar(1)", false},
      {"CARDINALITY", kIntegerType, false},       {"PAGES", kIntegerType, false},
      {"FILTER_CONDITION", kRemarksType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& specialColumnsLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"SCOPE", kSmallIntType, false},        {"COLUMN_NAME", kIdentifierType, true},
      {"DATA_TYPE", kSmallIntType, true},     {"TYPE_NAME", kTypeNameType, true},
      {"COLUMN_SIZE", kIntegerType, false},   {"BUFFER_LENGTH", kIntegerType, false},
      {"DECIMAL_DIGITS", kSmallIntType, false}, {"PSEUDO_COLUMN", kSmallIntType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& tablePrivilegesLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false}, {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, true}, {"GRANTOR", kIdentifierType, false},
      {"GRANTEE", kIdentifierType, true},    {"PRIVILEGE", kIdentifierType, true},
      {"IS_GRANTABLE", kFlagTextType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& columnPrivilegesLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"TABLE_CAT", kIdentifierType, false},  {"TABLE_SCHEM", kIdentifierType, false},
      {"TABLE_NAME", kIdentifierType, true},  {"COLUMN_NAME", kIdentifierType, true},
      {"GRANTOR", kIdentifierType, false},    {"GRANTEE", kIdentifierType, true},
      {"PRIVILEGE", kIdentifierType, true},   {"IS_GRANTABLE", kFlagTextType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& proceduresLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"PROCEDURE_CAT", kIdentifierType, false},  {"PROCEDURE_SCHEM", kIdentifierType, false},
      {"PROCEDURE_NAME", kIdentifierType, true},  {"NUM_INPUT_PARAMS", kIntegerType, false},
      {"NUM_OUTPUT_PARAMS", kIntegerType, false}, {"NUM_RESULT_SETS", kIntegerType, false},
      {"REMARKS", kRemarksType, false},           {"PROCEDURE_TYPE", kSmallIntType, false},
  };
  return layout;
}

const std::vector<ColumnSpec>& procedureColumnsLayout() {
  static const std::vector<ColumnSpec> layout = {
      {"PROCEDURE_CAT", kIdentifierType, false},   {"PROCEDURE_SCHEM", kIdentifierType, false},
      {"PROCEDURE_NAME", kIdentifierType, true},   {"COLUMN_NAME", kIdentifierType, true},
      {"COLUMN_TYPE", kSmallIntType, true},        {"DATA_TYPE", kSmallIntType, true},
      {"TYPE_NAME", kTypeNameType, true},          {"COLUMN_SIZE", kIntegerType, false},
      {"BUFFER_LENGTH", kIntegerType, false},      {"DECIMAL_DIGITS", kSmallIntType, false},
      {"NUM_PREC_RADIX", kSmallIntType, false},    {"NULLABLE", kSmallIntType, true},
      {"REMARKS", kRemarksType, false},            {"COLUMN_DEF", kRemarksType, false},
      {"SQL_DATA_TYPE", kSmallIntType, true},      {"SQL_DATETIME_SUB", kSmallIntType, false},
      {"CHAR_OCTET_LENGTH", kIntegerType, false},  {"ORDINAL_POSITION", kIntegerType, true},
      {"IS_NULLABLE", kFlagTextType, false},
  };
  return layout;
}

/// ResultColumns for a layout, with the spec's NOT NULL columns marked SQL_NO_NULLS.
std::vector<ResultColumn> makeColumns(const std::vector<ColumnSpec>& layout, const TypeMappingOptions& opts) {
  std::vector<ResultColumn> columns;
  columns.reserve(layout.size());
  for (const ColumnSpec& spec : layout) {
    ResultColumn c = ResultColumn::make(spec.name, spec.trinoType, opts);
    c.odbc.nullable = spec.notNull ? SQL_NO_NULLS : SQL_NULLABLE;
    columns.push_back(std::move(c));
  }
  return columns;
}

std::vector<StorageKind> kindsOf(const std::vector<ResultColumn>& columns) {
  std::vector<StorageKind> kinds;
  kinds.reserve(columns.size());
  for (const ResultColumn& c : columns) kinds.push_back(c.odbc.storage);
  return kinds;
}

/// The type mapping in force for this connection (config-derived when no client exists yet, so
/// the empty result sets can be built without a live connection).
TypeMappingOptions mappingOf(const Connection& conn) {
  return conn.client ? conn.client->typeMapping() : conn.config.typeMapping();
}

std::unique_ptr<MemoryRowSource> emptyResult(const std::vector<ColumnSpec>& layout, const Connection& conn,
                                             const char* function) {
  TRINO_LOG_DEBUG(kLog, "%s: returning an empty result set (%zu columns)", function, layout.size());
  return std::make_unique<MemoryRowSource>(makeColumns(layout, mappingOf(conn)));
}

// ---- cells ---------------------------------------------------------------------------------------

Cell nullCell() { return Cell{}; }
Cell intCell(int64_t v) { return Cell{v}; }
Cell textCell(std::string v) { return Cell{std::move(v)}; }

/// Clamps a length reported by the type mapping into the 32-bit INTEGER column of the result.
int64_t clampToInteger(int64_t v) {
  const int64_t max = std::numeric_limits<int32_t>::max();
  return v < 0 ? 0 : (v > max ? max : v);
}

// ---- reading the server's rows -------------------------------------------------------------------

/// Resolves the columns of a system.jdbc result by name so the driver keeps working if Trino ever
/// reorders or extends the JDBC tables.
class ColumnIndex {
 public:
  ColumnIndex(const std::vector<ResultColumn>& columns, const char* source) : columns_(columns), source_(source) {}

  /// Index of a column that must exist; HY000 otherwise, because a missing column means the
  /// server is not speaking the system.jdbc contract and guessing would corrupt metadata.
  size_t require(std::string_view name) const {
    const int idx = find(name);
    if (idx < 0) {
      throw DriverError(sqlstate::kGeneralError,
                        std::string(source_) + " did not return the column '" + std::string(name) + "'");
    }
    return static_cast<size_t>(idx);
  }

  /// Index of an optional column, or -1.
  int find(std::string_view name) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
      if (iequals(columns_[i].name, name)) return static_cast<int>(i);
    }
    return -1;
  }

 private:
  const std::vector<ResultColumn>& columns_;
  const char* source_;
};

/// Cell text of any storage kind (NULL -> monostate).
Cell readText(const Column& col, size_t row) {
  if (col.isNull(row)) return nullCell();
  switch (col.kind()) {
    case StorageKind::String:
    case StorageKind::Binary: return textCell(std::string(col.getBytes(row)));
    case StorageKind::Int64: return textCell(std::to_string(col.getInt64(row)));
    case StorageKind::Bool: return textCell(col.getBool(row) ? "true" : "false");
    case StorageKind::Double: return textCell(formatString("%.17g", col.getDouble(row)));
  }
  return nullCell();
}

std::string readString(const Column& col, size_t row) {
  const Cell c = readText(col, row);
  const std::string* s = std::get_if<std::string>(&c);
  return s ? *s : std::string();
}

/// Integer value of a cell whatever the server's storage (NULL / unparsable -> nullopt).
std::optional<int64_t> readInt(const Column& col, size_t row) {
  if (col.isNull(row)) return std::nullopt;
  switch (col.kind()) {
    case StorageKind::Int64: return col.getInt64(row);
    case StorageKind::Bool: return col.getBool(row) ? 1 : 0;
    case StorageKind::Double: {
      const double d = col.getDouble(row);
      if (!std::isfinite(d)) return std::nullopt;
      return static_cast<int64_t>(d);
    }
    case StorageKind::String:
    case StorageKind::Binary: {
      int64_t v = 0;
      if (parseInt64(trim(col.getBytes(row)), &v)) return v;
      return std::nullopt;
    }
  }
  return std::nullopt;
}

Cell readIntCell(const Column& col, size_t row) {
  const std::optional<int64_t> v = readInt(col, row);
  return v ? intCell(*v) : nullCell();
}

Cell readIntCell(const Page& page, int index, size_t row) {
  return index < 0 ? nullCell() : readIntCell(page.columns[static_cast<size_t>(index)], row);
}

Cell readTextCell(const Page& page, int index, size_t row) {
  return index < 0 ? nullCell() : readText(page.columns[static_cast<size_t>(index)], row);
}

/// Runs one metadata query on the connection (internal options: no spooling, no prefetch thread).
std::unique_ptr<MemoryRowSource> runMetadataQuery(Connection& conn, const std::string& sql, const char* function) {
  if (!conn.connected || !conn.client) {
    throw DriverError(sqlstate::kNotConnected, std::string(function) + " requires an open connection");
  }
  ExecOptions opts;
  opts.internal = true;
  // SQLCancel from another thread sets the statement's flag; the internal query polls it through
  // its whole run (including the paging inside executeToMemory) and fails with HY008.
  opts.cancelFlag = MetadataCancelScope::current();
  if (opts.cancelFlag && opts.cancelFlag->load()) throw CancelledError();
  TRINO_LOG_DEBUG(kLog, "%s: %s", function, sql.c_str());
  std::unique_ptr<MemoryRowSource> rows = conn.client->executeToMemory(sql, opts);
  if (!rows) {
    throw DriverError(sqlstate::kGeneralError, std::string(function) + ": the metadata query produced no result");
  }
  TRINO_LOG_DEBUG(kLog, "%s: %llu rows from the server", function,
                  static_cast<unsigned long long>(rows->totalRows()));
  return rows;
}

// ---- type-derived columns ------------------------------------------------------------------------

/// The columns of SQLColumns / SQLProcedureColumns that depend on the data type, computed from the
/// Trino type name with the same mapping SQLDescribeCol uses.
struct TypeCells {
  Cell dataType, typeName, columnSize, bufferLength, decimalDigits, numPrecRadix, sqlDataType, sqlDatetimeSub,
      charOctetLength;
};

bool isCharacterOrBinaryType(SQLSMALLINT sqlType) {
  switch (sqlType) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return true;
    default:
      return false;
  }
}

/// DECIMAL_DIGITS per ODBC Appendix D: 0 for integers and bit, the scale for decimals, the
/// fractional-second digits for time/timestamp and second-bearing intervals, NULL elsewhere.
Cell decimalDigitsCell(const OdbcTypeInfo& info) {
  switch (info.conciseType) {
    case SQL_BIT:
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_BIGINT:
      return intCell(0);
    case SQL_DECIMAL:
    case SQL_NUMERIC:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      return intCell(info.decimalDigits);
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
      return intCell(0);
    default:
      return nullCell();
  }
}

TypeCells typeCellsFor(std::string_view trinoTypeName, const TypeMappingOptions& opts, const Connection* conn) {
  const TrinoType type = TrinoType::parse(trinoTypeName);
  const OdbcTypeInfo info = mapTrinoType(type, opts);
  TypeCells cells;
  // ODBC 2.x applications expect SQL_DATE/SQL_TIME/SQL_TIMESTAMP (9/10/11) instead of the ODBC 3 ids.
  cells.dataType = intCell(sqlTypeForApplication(conn, info.conciseType));
  // The canonical spelling ("varchar(25)", "timestamp(3) with time zone") rather than the server's
  // text, so SQLColumns and SQL_DESC_TYPE_NAME print the same thing.
  cells.typeName = textCell(info.typeName.empty() ? std::string(trinoTypeName) : info.typeName);
  cells.columnSize = intCell(clampToInteger(static_cast<int64_t>(info.columnSize)));
  cells.bufferLength = intCell(clampToInteger(static_cast<int64_t>(info.octetLength)));
  cells.decimalDigits = decimalDigitsCell(info);
  cells.numPrecRadix = info.numPrecRadix != 0 ? intCell(info.numPrecRadix) : nullCell();
  cells.sqlDataType = intCell(info.verboseType);
  if (info.datetimeSubcode != 0) {
    cells.sqlDatetimeSub = intCell(info.datetimeSubcode);
  } else if (info.intervalType && info.intervalCode != 0) {
    cells.sqlDatetimeSub = intCell(info.intervalCode);
  } else {
    cells.sqlDatetimeSub = nullCell();
  }
  cells.charOctetLength = isCharacterOrBinaryType(info.conciseType)
                              ? intCell(clampToInteger(static_cast<int64_t>(info.octetLength)))
                              : nullCell();
  return cells;
}

/// ODBC NULLABLE from the JDBC nullable code (0 = no nulls, 1 = nullable, 2 = unknown).
int64_t nullableCode(const std::optional<int64_t>& jdbcNullable) {
  if (!jdbcNullable) return SQL_NULLABLE_UNKNOWN;
  switch (*jdbcNullable) {
    case 0: return SQL_NO_NULLS;
    case 1: return SQL_NULLABLE;
    default: return SQL_NULLABLE_UNKNOWN;
  }
}

/// IS_NULLABLE derived from NULLABLE so both columns always agree ("" = unknown, per spec).
std::string isNullableText(int64_t nullable) {
  if (nullable == SQL_NO_NULLS) return "NO";
  if (nullable == SQL_NULLABLE) return "YES";
  return std::string();
}

}  // namespace

// ==================================================================================================
// catalog_sql (declared in catalog_sql.h) and patternPredicate (declared in catalog.h)
// ==================================================================================================

std::string patternPredicate(std::string_view columnExpr, const std::optional<std::string>& pattern, bool literal) {
  if (!pattern) return std::string();  // null pointer: the argument is not a filter
  std::string out(columnExpr);
  if (literal) {
    out.append(" = ").append(sqlQuoteLiteral(catalog_sql::normalizeIdentifierArgument(*pattern)));
    return out;
  }
  if (*pattern == "%") return std::string();  // matches everything: no predicate needed
  if (pattern->empty()) {
    // ODBC: an empty string names objects that have no such qualifier. Trino always qualifies,
    // so this matches nothing (which is the correct answer, not an error).
    out.append(" = ''");
    return out;
  }
  out.append(" LIKE ").append(likeLiteral(*pattern)).append(" ESCAPE '\\'");
  return out;
}

namespace catalog_sql {

SpecialEnumeration detectSpecialEnumeration(const CatalogArgs& args) {
  if (isAll(args.catalog) && isEmpty(args.schema) && isEmpty(args.table)) return SpecialEnumeration::AllCatalogs;
  if (isAll(args.schema) && isEmpty(args.table)) {
    return isEmpty(args.catalog) ? SpecialEnumeration::AllSchemas : SpecialEnumeration::CatalogSchemas;
  }
  if (isAll(args.tableTypes) && isEmpty(args.catalog) && isEmpty(args.schema) && isEmpty(args.table)) {
    return SpecialEnumeration::AllTableTypes;
  }
  return SpecialEnumeration::None;
}

std::vector<std::string> parseTableTypes(const std::optional<std::string>& tableTypes) {
  std::vector<std::string> types;
  if (!tableTypes) return types;
  const std::string_view text = trim(*tableTypes);
  if (text.empty() || text == "%") return types;
  for (const std::string& rawItem : split(text, ',', true, true)) {
    const std::string canonical = canonicalTableType(trim(unquoteSingle(trim(rawItem))));
    if (canonical.empty()) continue;
    if (canonical == "%") return {};  // SQL_ALL_TABLE_TYPES inside a list: no filter at all
    if (std::find(types.begin(), types.end(), canonical) == types.end()) types.push_back(canonical);
  }
  return types;
}

std::string normalizeIdentifierArgument(std::string_view value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return replaceAll(value.substr(1, value.size() - 2), "\"\"", "\"");
  }
  size_t end = value.size();
  while (end > 0 && value[end - 1] == ' ') --end;
  return std::string(value.substr(0, end));
}

std::string ordinaryPredicate(std::string_view columnExpr, const std::optional<std::string>& value, bool literal) {
  if (!value) return std::string();
  std::string out(columnExpr);
  out.append(" = ").append(sqlQuoteLiteral(literal ? normalizeIdentifierArgument(*value) : *value));
  return out;
}

std::string tableTypesPredicate(const std::vector<std::string>& types) {
  if (types.empty()) return std::string();
  std::string out = "table_type IN (";
  for (size_t i = 0; i < types.size(); ++i) {
    if (i > 0) out.append(", ");
    out.append(sqlQuoteLiteral(types[i]));
  }
  out.push_back(')');
  return out;
}

std::string whereClause(const std::vector<std::string>& predicates) {
  std::string out;
  for (const std::string& p : predicates) {
    if (p.empty()) continue;
    out.append(out.empty() ? " WHERE " : " AND ").append(p);
  }
  return out;
}

std::string buildCatalogsQuery() { return "SELECT table_cat FROM system.jdbc.catalogs ORDER BY table_cat"; }

std::string buildSchemasQuery(const CatalogArgs& args) {
  const bool lit = args.identifiersAreLiterals;
  return "SELECT table_schem, table_catalog FROM system.jdbc.schemas" +
         whereClause({patternPredicate("table_catalog", args.catalog, lit),
                      patternPredicate("table_schem", args.schema, lit)}) +
         " ORDER BY table_catalog, table_schem";
}

std::string buildTableTypesQuery() { return "SELECT table_type FROM system.jdbc.table_types ORDER BY table_type"; }

std::string buildTablesQuery(const CatalogArgs& args) {
  const bool lit = args.identifiersAreLiterals;
  // ODBC 3 lets SQLTables' CatalogName carry a search pattern, hence LIKE for all three names.
  return "SELECT table_cat, table_schem, table_name, table_type, remarks, type_cat, type_schem, type_name, "
         "self_referencing_col_name, ref_generation FROM system.jdbc.tables" +
         whereClause({patternPredicate("table_cat", args.catalog, lit),
                      patternPredicate("table_schem", args.schema, lit),
                      patternPredicate("table_name", args.table, lit),
                      tableTypesPredicate(parseTableTypes(args.tableTypes))}) +
         " ORDER BY table_type, table_cat, table_schem, table_name";
}

std::string buildColumnsQuery(const CatalogArgs& args) {
  const bool lit = args.identifiersAreLiterals;
  // CatalogName is an ordinary argument for SQLColumns (no pattern), the other three are patterns.
  return "SELECT table_cat, table_schem, table_name, column_name, data_type, type_name, column_size, "
         "buffer_length, decimal_digits, num_prec_radix, nullable, remarks, column_def, sql_data_type, "
         "sql_datetime_sub, char_octet_length, ordinal_position, is_nullable FROM system.jdbc.columns" +
         whereClause({ordinaryPredicate("table_cat", args.catalog, lit),
                      patternPredicate("table_schem", args.schema, lit),
                      patternPredicate("table_name", args.table, lit),
                      patternPredicate("column_name", args.column, lit)}) +
         " ORDER BY table_cat, table_schem, table_name, ordinal_position";
}

std::string buildProceduresQuery(const CatalogArgs& args) {
  const bool lit = args.identifiersAreLiterals;
  return "SELECT procedure_cat, procedure_schem, procedure_name, remarks, procedure_type, specific_name "
         "FROM system.jdbc.procedures" +
         whereClause({ordinaryPredicate("procedure_cat", args.catalog, lit),
                      patternPredicate("procedure_schem", args.schema, lit),
                      patternPredicate("procedure_name", args.table, lit)}) +
         " ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name";
}

std::string buildProcedureColumnsQuery(const CatalogArgs& args) {
  const bool lit = args.identifiersAreLiterals;
  return "SELECT procedure_cat, procedure_schem, procedure_name, column_name, column_type, data_type, type_name, "
         "precision, length, scale, radix, nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
         "char_octet_length, ordinal_position, is_nullable, specific_name FROM system.jdbc.procedure_columns" +
         whereClause({ordinaryPredicate("procedure_cat", args.catalog, lit),
                      patternPredicate("procedure_schem", args.schema, lit),
                      patternPredicate("procedure_name", args.table, lit),
                      patternPredicate("column_name", args.column, lit)}) +
         " ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name, ordinal_position";
}

}  // namespace catalog_sql

// ==================================================================================================
// SQLTables
// ==================================================================================================

namespace {

/// TABLE_CAT list for SQL_ALL_CATALOGS (every other column NULL, as the spec prescribes).
void addCatalogRows(Connection& conn, PageBuilder& out) {
  std::unique_ptr<MemoryRowSource> rows = runMetadataQuery(conn, catalog_sql::buildCatalogsQuery(), "SQLTables");
  const ColumnIndex idx(rows->columns(), "system.jdbc.catalogs");
  const size_t cat = idx.require("table_cat");
  while (std::unique_ptr<Page> page = rows->nextPage()) {
    for (size_t r = 0; r < page->rowCount; ++r) {
      out.addRow({readText(page->columns[cat], r), nullCell(), nullCell(), nullCell(), nullCell()});
    }
  }
}

/// TABLE_SCHEM list. For the spec enumeration (CatalogName = "") the catalog column stays NULL and
/// duplicates across catalogs are collapsed; for the lenient form the catalog is reported.
void addSchemaRows(Connection& conn, const CatalogArgs& args, bool withCatalog, PageBuilder& out) {
  CatalogArgs query = args;
  query.schema.reset();                   // "%" already means "all"
  query.table.reset();
  query.tableTypes.reset();
  if (!withCatalog) query.catalog.reset();  // "" was the trigger, not a filter
  std::unique_ptr<MemoryRowSource> rows = runMetadataQuery(conn, catalog_sql::buildSchemasQuery(query), "SQLTables");
  const ColumnIndex idx(rows->columns(), "system.jdbc.schemas");
  const size_t schem = idx.require("table_schem");
  const size_t cat = idx.require("table_catalog");
  std::set<std::string> distinct;
  while (std::unique_ptr<Page> page = rows->nextPage()) {
    for (size_t r = 0; r < page->rowCount; ++r) {
      if (withCatalog) {
        out.addRow({readText(page->columns[cat], r), readText(page->columns[schem], r), nullCell(), nullCell(),
                    nullCell()});
      } else {
        distinct.insert(readString(page->columns[schem], r));
      }
    }
  }
  // std::set orders by byte value, which is also Trino's ordering for varchar.
  for (const std::string& name : distinct) {
    out.addRow({nullCell(), textCell(name), nullCell(), nullCell(), nullCell()});
  }
}

/// TABLE_TYPE list for SQL_ALL_TABLE_TYPES.
void addTableTypeRows(Connection& conn, PageBuilder& out) {
  std::unique_ptr<MemoryRowSource> rows = runMetadataQuery(conn, catalog_sql::buildTableTypesQuery(), "SQLTables");
  const ColumnIndex idx(rows->columns(), "system.jdbc.table_types");
  const size_t type = idx.require("table_type");
  while (std::unique_ptr<Page> page = rows->nextPage()) {
    for (size_t r = 0; r < page->rowCount; ++r) {
      out.addRow({nullCell(), nullCell(), nullCell(), readText(page->columns[type], r), nullCell()});
    }
  }
}

void addTableRows(Connection& conn, const CatalogArgs& args, PageBuilder& out, std::vector<QueryWarning>& warnings) {
  std::unique_ptr<MemoryRowSource> rows = runMetadataQuery(conn, catalog_sql::buildTablesQuery(args), "SQLTables");
  const ColumnIndex idx(rows->columns(), "system.jdbc.tables");
  const size_t cat = idx.require("table_cat");
  const size_t schem = idx.require("table_schem");
  const size_t name = idx.require("table_name");
  const size_t type = idx.require("table_type");
  const int remarks = idx.find("remarks");
  while (std::unique_ptr<Page> page = rows->nextPage()) {
    for (size_t r = 0; r < page->rowCount; ++r) {
      out.addRow({readText(page->columns[cat], r), readText(page->columns[schem], r), readText(page->columns[name], r),
                  readText(page->columns[type], r), readTextCell(*page, remarks, r)});
    }
  }
  warnings = rows->warnings();
}

}  // namespace

std::unique_ptr<MemoryRowSource> catalogTables(Connection& conn, const CatalogArgs& args) {
  std::vector<ResultColumn> columns = makeColumns(tablesLayout(), mappingOf(conn));
  PageBuilder builder(kindsOf(columns));
  std::vector<QueryWarning> warnings;
  const catalog_sql::SpecialEnumeration mode = catalog_sql::detectSpecialEnumeration(args);
  switch (mode) {
    case catalog_sql::SpecialEnumeration::AllCatalogs:
      TRINO_LOG_DEBUG(kLog, "SQLTables: SQL_ALL_CATALOGS enumeration");
      addCatalogRows(conn, builder);
      break;
    case catalog_sql::SpecialEnumeration::AllSchemas:
      TRINO_LOG_DEBUG(kLog, "SQLTables: SQL_ALL_SCHEMAS enumeration");
      addSchemaRows(conn, args, /*withCatalog=*/false, builder);
      break;
    case catalog_sql::SpecialEnumeration::CatalogSchemas:
      TRINO_LOG_DEBUG(kLog, "SQLTables: schema enumeration for catalog '%s'",
                      args.catalog ? args.catalog->c_str() : "<null>");
      addSchemaRows(conn, args, /*withCatalog=*/true, builder);
      break;
    case catalog_sql::SpecialEnumeration::AllTableTypes:
      TRINO_LOG_DEBUG(kLog, "SQLTables: SQL_ALL_TABLE_TYPES enumeration");
      addTableTypeRows(conn, builder);
      break;
    case catalog_sql::SpecialEnumeration::None:
      addTableRows(conn, args, builder, warnings);
      break;
  }
  TRINO_LOG_DEBUG(kLog, "SQLTables: %zu rows", builder.rowCount());
  auto result = std::make_unique<MemoryRowSource>(std::move(columns), builder.finish());
  result->setWarnings(std::move(warnings));
  return result;
}

// ==================================================================================================
// SQLColumns
// ==================================================================================================

std::unique_ptr<MemoryRowSource> catalogColumns(Connection& conn, const CatalogArgs& args) {
  const TypeMappingOptions opts = mappingOf(conn);
  std::vector<ResultColumn> columns = makeColumns(columnsLayout(), opts);
  PageBuilder builder(kindsOf(columns));

  std::unique_ptr<MemoryRowSource> rows = runMetadataQuery(conn, catalog_sql::buildColumnsQuery(args), "SQLColumns");
  const ColumnIndex idx(rows->columns(), "system.jdbc.columns");
  const size_t cat = idx.require("table_cat");
  const size_t schem = idx.require("table_schem");
  const size_t table = idx.require("table_name");
  const size_t column = idx.require("column_name");
  const size_t typeName = idx.require("type_name");
  const size_t ordinal = idx.require("ordinal_position");
  const int nullable = idx.find("nullable");
  const int remarks = idx.find("remarks");
  const int columnDef = idx.find("column_def");

  size_t sequence = 0;  // fallback ORDINAL_POSITION when the server leaves it NULL
  while (std::unique_ptr<Page> page = rows->nextPage()) {
    for (size_t r = 0; r < page->rowCount; ++r) {
      ++sequence;
      const TypeCells type = typeCellsFor(readString(page->columns[typeName], r), opts, &conn);
      const int64_t nullableValue = nullableCode(nullable < 0 ? std::nullopt
                                                              : readInt(page->columns[static_cast<size_t>(nullable)], r));
      const std::optional<int64_t> position = readInt(page->columns[ordinal], r);
      builder.addRow({readText(page->columns[cat], r), readText(page->columns[schem], r),
                      readText(page->columns[table], r), readText(page->columns[column], r), type.dataType,
                      type.typeName, type.columnSize, type.bufferLength, type.decimalDigits, type.numPrecRadix,
                      intCell(nullableValue), readTextCell(*page, remarks, r), readTextCell(*page, columnDef, r),
                      type.sqlDataType, type.sqlDatetimeSub, type.charOctetLength,
                      intCell(position ? *position : static_cast<int64_t>(sequence)),
                      textCell(isNullableText(nullableValue))});
    }
  }
  TRINO_LOG_DEBUG(kLog, "SQLColumns: %zu rows", builder.rowCount());
  auto result = std::make_unique<MemoryRowSource>(std::move(columns), builder.finish());
  result->setWarnings(rows->warnings());
  return result;
}

// ==================================================================================================
// Empty result sets (Trino has no keys, indexes or grants in its metadata)
// ==================================================================================================

std::unique_ptr<MemoryRowSource> catalogPrimaryKeys(Connection& conn, const CatalogArgs& args) {
  TRINO_LOG_DEBUG(kLog, "SQLPrimaryKeys(%s): Trino reports no primary keys",
                  args.table ? args.table->c_str() : "<null>");
  return emptyResult(primaryKeysLayout(), conn, "SQLPrimaryKeys");
}

std::unique_ptr<MemoryRowSource> catalogForeignKeys(Connection& conn, const CatalogArgs& pk, const CatalogArgs& fk) {
  TRINO_LOG_DEBUG(kLog, "SQLForeignKeys(pk=%s, fk=%s): Trino reports no foreign keys",
                  pk.table ? pk.table->c_str() : "<null>", fk.table ? fk.table->c_str() : "<null>");
  return emptyResult(foreignKeysLayout(), conn, "SQLForeignKeys");
}

std::unique_ptr<MemoryRowSource> catalogStatistics(Connection& conn, const CatalogArgs& args, SQLUSMALLINT unique,
                                                   SQLUSMALLINT reserved) {
  if (unique != SQL_INDEX_UNIQUE && unique != SQL_INDEX_ALL) {
    throw DriverError(sqlstate::kUniquenessOptionOutOfRange,
                      "SQLStatistics: invalid Unique argument " + std::to_string(unique));
  }
  if (reserved != SQL_ENSURE && reserved != SQL_QUICK) {
    throw DriverError(sqlstate::kAccuracyOptionOutOfRange,
                      "SQLStatistics: invalid Reserved argument " + std::to_string(reserved));
  }
  TRINO_LOG_DEBUG(kLog, "SQLStatistics(%s): Trino reports no indexes", args.table ? args.table->c_str() : "<null>");
  return emptyResult(statisticsLayout(), conn, "SQLStatistics");
}

std::unique_ptr<MemoryRowSource> catalogSpecialColumns(Connection& conn, SQLUSMALLINT idType, const CatalogArgs& args,
                                                       SQLUSMALLINT scope, SQLUSMALLINT nullable) {
  if (idType != SQL_BEST_ROWID && idType != SQL_ROWVER) {
    throw DriverError(sqlstate::kColumnTypeOutOfRange,
                      "SQLSpecialColumns: invalid IdentifierType argument " + std::to_string(idType));
  }
  if (scope != SQL_SCOPE_CURROW && scope != SQL_SCOPE_TRANSACTION && scope != SQL_SCOPE_SESSION) {
    throw DriverError(sqlstate::kScopeTypeOutOfRange,
                      "SQLSpecialColumns: invalid Scope argument " + std::to_string(scope));
  }
  if (nullable != SQL_NO_NULLS && nullable != SQL_NULLABLE) {
    throw DriverError(sqlstate::kNullableTypeOutOfRange,
                      "SQLSpecialColumns: invalid Nullable argument " + std::to_string(nullable));
  }
  TRINO_LOG_DEBUG(kLog, "SQLSpecialColumns(%s): Trino has no row identifiers or version columns",
                  args.table ? args.table->c_str() : "<null>");
  return emptyResult(specialColumnsLayout(), conn, "SQLSpecialColumns");
}

std::unique_ptr<MemoryRowSource> catalogTablePrivileges(Connection& conn, const CatalogArgs& args) {
  TRINO_LOG_DEBUG(kLog, "SQLTablePrivileges(%s): grants are not exposed by system.jdbc",
                  args.table ? args.table->c_str() : "<null>");
  return emptyResult(tablePrivilegesLayout(), conn, "SQLTablePrivileges");
}

std::unique_ptr<MemoryRowSource> catalogColumnPrivileges(Connection& conn, const CatalogArgs& args) {
  TRINO_LOG_DEBUG(kLog, "SQLColumnPrivileges(%s): grants are not exposed by system.jdbc",
                  args.table ? args.table->c_str() : "<null>");
  return emptyResult(columnPrivilegesLayout(), conn, "SQLColumnPrivileges");
}

// ==================================================================================================
// SQLProcedures / SQLProcedureColumns
// ==================================================================================================

namespace {

/// Runs a system.jdbc.procedures* query; a server that lacks the table (or denies access to it)
/// yields an empty result rather than an error, because "no procedures" is the truthful answer.
std::unique_ptr<MemoryRowSource> runProcedureQuery(Connection& conn, const std::string& sql, const char* function) {
  try {
    return runMetadataQuery(conn, sql, function);
  } catch (const TrinoQueryError& e) {
    TRINO_LOG_INFO(kLog, "%s: server has no usable procedure metadata (%s: %s); returning an empty result", function,
                   e.errorName().c_str(), e.message().c_str());
    return nullptr;
  }
}

}  // namespace

std::unique_ptr<MemoryRowSource> catalogProcedures(Connection& conn, const CatalogArgs& args) {
  std::vector<ResultColumn> columns = makeColumns(proceduresLayout(), mappingOf(conn));
  PageBuilder builder(kindsOf(columns));
  std::unique_ptr<MemoryRowSource> rows = runProcedureQuery(conn, catalog_sql::buildProceduresQuery(args), "SQLProcedures");
  if (rows) {
    const ColumnIndex idx(rows->columns(), "system.jdbc.procedures");
    const size_t cat = idx.require("procedure_cat");
    const size_t schem = idx.require("procedure_schem");
    const size_t name = idx.require("procedure_name");
    const int remarks = idx.find("remarks");
    const int procType = idx.find("procedure_type");
    while (std::unique_ptr<Page> page = rows->nextPage()) {
      for (size_t r = 0; r < page->rowCount; ++r) {
        // JDBC procedure_type codes (0 unknown, 1 no result, 2 returns result) are numerically
        // identical to SQL_PT_UNKNOWN / SQL_PT_PROCEDURE / SQL_PT_FUNCTION.
        Cell type = readIntCell(*page, procType, r);
        if (const int64_t* v = std::get_if<int64_t>(&type)) {
          if (*v < SQL_PT_UNKNOWN || *v > SQL_PT_FUNCTION) type = intCell(SQL_PT_UNKNOWN);
        }
        builder.addRow({readText(page->columns[cat], r), readText(page->columns[schem], r),
                        readText(page->columns[name], r), nullCell(), nullCell(), nullCell(),
                        readTextCell(*page, remarks, r), type});
      }
    }
  }
  TRINO_LOG_DEBUG(kLog, "SQLProcedures: %zu rows", builder.rowCount());
  return std::make_unique<MemoryRowSource>(std::move(columns), builder.finish());
}

std::unique_ptr<MemoryRowSource> catalogProcedureColumns(Connection& conn, const CatalogArgs& args) {
  const TypeMappingOptions opts = mappingOf(conn);
  std::vector<ResultColumn> columns = makeColumns(procedureColumnsLayout(), opts);
  PageBuilder builder(kindsOf(columns));
  std::unique_ptr<MemoryRowSource> rows =
      runProcedureQuery(conn, catalog_sql::buildProcedureColumnsQuery(args), "SQLProcedureColumns");
  if (rows) {
    const ColumnIndex idx(rows->columns(), "system.jdbc.procedure_columns");
    const size_t cat = idx.require("procedure_cat");
    const size_t schem = idx.require("procedure_schem");
    const size_t name = idx.require("procedure_name");
    const size_t column = idx.require("column_name");
    const size_t typeName = idx.require("type_name");
    const int columnType = idx.find("column_type");
    const int nullable = idx.find("nullable");
    const int remarks = idx.find("remarks");
    const int columnDef = idx.find("column_def");
    const int ordinal = idx.find("ordinal_position");
    size_t sequence = 0;
    while (std::unique_ptr<Page> page = rows->nextPage()) {
      for (size_t r = 0; r < page->rowCount; ++r) {
        ++sequence;
        const TypeCells type = typeCellsFor(readString(page->columns[typeName], r), opts, &conn);
        const int64_t nullableValue = nullableCode(
            nullable < 0 ? std::nullopt : readInt(page->columns[static_cast<size_t>(nullable)], r));
        // JDBC column_type codes (unknown 0, in 1, in/out 2, result 3, out 4, return 5) coincide
        // with SQL_PARAM_TYPE_UNKNOWN .. SQL_RETURN_VALUE.
        Cell kind = readIntCell(*page, columnType, r);
        if (const int64_t* v = std::get_if<int64_t>(&kind)) {
          if (*v < SQL_PARAM_TYPE_UNKNOWN || *v > SQL_RETURN_VALUE) kind = intCell(SQL_PARAM_TYPE_UNKNOWN);
        } else {
          kind = intCell(SQL_PARAM_TYPE_UNKNOWN);
        }
        const std::optional<int64_t> position =
            ordinal < 0 ? std::nullopt : readInt(page->columns[static_cast<size_t>(ordinal)], r);
        builder.addRow({readText(page->columns[cat], r), readText(page->columns[schem], r),
                        readText(page->columns[name], r), readText(page->columns[column], r), kind, type.dataType,
                        type.typeName, type.columnSize, type.bufferLength, type.decimalDigits, type.numPrecRadix,
                        intCell(nullableValue), readTextCell(*page, remarks, r), readTextCell(*page, columnDef, r),
                        type.sqlDataType, type.sqlDatetimeSub, type.charOctetLength,
                        intCell(position ? *position : static_cast<int64_t>(sequence)),
                        textCell(isNullableText(nullableValue))});
      }
    }
  }
  TRINO_LOG_DEBUG(kLog, "SQLProcedureColumns: %zu rows", builder.rowCount());
  return std::make_unique<MemoryRowSource>(std::move(columns), builder.finish());
}

}  // namespace trino_odbc
