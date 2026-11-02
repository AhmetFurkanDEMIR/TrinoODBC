// Diagnostics area: record ordering, message formatting, Trino error-name -> SQLSTATE mapping and
// the dynamic-function classifier used for SQL_DIAG_DYNAMIC_FUNCTION(_CODE). See ARCHITECTURE.md §8.
#include "core/diag.h"

#include "util/strings.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace trino_odbc {

namespace {

constexpr const char* kDriverPrefix = "[Trino][ODBC Driver] ";
constexpr const char* kServerPrefix = "[Trino][ODBC Driver][Trino] ";

/// Severity rank used by mergeReturnCode: higher wins.
int returnCodeRank(SQLRETURN rc) {
  switch (rc) {
    case SQL_SUCCESS: return 0;
    case SQL_SUCCESS_WITH_INFO: return 1;
    case SQL_STILL_EXECUTING: return 2;
    case SQL_NO_DATA: return 3;
    case SQL_NEED_DATA: return 4;
    case SQL_ERROR: return 5;
    case SQL_INVALID_HANDLE: return 6;
    default: return 5;  // unknown codes are treated as errors so they are never masked
  }
}

struct ErrorNameEntry {
  const char* name;
  const char* sqlstate;
};

// Explicit Trino StandardErrorCode names. Names not listed fall back to the prefix rules and then
// to the errorType default. Keep the entries from ARCHITECTURE.md §8 first; the rest follow Trino's
// io.trino.spi.StandardErrorCode and the closest ODBC / SQL-92 SQLSTATE.
constexpr ErrorNameEntry kErrorNames[] = {
    // --- entries mandated by the architecture contract
    {"SYNTAX_ERROR", "42000"},
    {"TABLE_NOT_FOUND", "42S02"},
    {"COLUMN_NOT_FOUND", "42S22"},
    {"SCHEMA_NOT_FOUND", "3F000"},
    {"CATALOG_NOT_FOUND", "3D000"},
    {"PERMISSION_DENIED", "42501"},
    {"USER_CANCELED", "HY008"},
    {"EXCEEDED_TIME_LIMIT", "HYT00"},
    {"DIVISION_BY_ZERO", "22012"},
    {"NUMERIC_VALUE_OUT_OF_RANGE", "22003"},
    {"INVALID_CAST_ARGUMENT", "22018"},
    {"INVALID_FUNCTION_ARGUMENT", "22023"},
    {"TYPE_MISMATCH", "42804"},
    {"AMBIGUOUS_NAME", "42702"},
    // --- additional user errors
    {"GENERIC_USER_ERROR", "HY000"},
    {"ABANDONED_QUERY", "HY008"},
    {"ADMINISTRATIVELY_KILLED", "HY008"},
    {"ADMINISTRATIVELY_PREEMPTED", "HY008"},
    {"NOT_FOUND", "42S02"},
    {"VIEW_NOT_FOUND", "42S02"},
    {"MATERIALIZED_VIEW_NOT_FOUND", "42S02"},
    {"FUNCTION_NOT_FOUND", "42883"},
    {"OPERATOR_NOT_FOUND", "42883"},
    {"PROCEDURE_NOT_FOUND", "42883"},
    {"TYPE_NOT_FOUND", "42704"},
    {"ROLE_NOT_FOUND", "42000"},
    {"ALREADY_EXISTS", "42S01"},
    {"TABLE_ALREADY_EXISTS", "42S01"},
    {"VIEW_ALREADY_EXISTS", "42S01"},
    {"MATERIALIZED_VIEW_ALREADY_EXISTS", "42S01"},
    {"SCHEMA_ALREADY_EXISTS", "42000"},
    {"ROLE_ALREADY_EXISTS", "42000"},
    {"FUNCTION_ALREADY_EXISTS", "42000"},
    {"COLUMN_ALREADY_EXISTS", "42S21"},
    {"DUPLICATE_COLUMN_NAME", "42S21"},
    {"DUPLICATE_NAMED_QUERY", "42000"},
    {"DUPLICATE_PROPERTY", "42000"},
    {"DUPLICATE_RANGE_VARIABLE", "42000"},
    {"DUPLICATE_PARAMETER_NAME", "42000"},
    {"DUPLICATE_WINDOW_NAME", "42000"},
    {"DUPLICATE_COLUMN_OR_PATH_NAME", "42000"},
    {"NOT_SUPPORTED", "0A000"},
    {"UNSUPPORTED_ISOLATION_LEVEL", "0A000"},
    {"MULTI_CATALOG_WRITE_CONFLICT", "0A000"},
    {"UNSUPPORTED_SUBQUERY", "0A000"},
    {"UNSUPPORTED_TABLE_TYPE", "0A000"},
    {"CONSTRAINT_VIOLATION", "23000"},
    {"TRANSACTION_CONFLICT", "40001"},
    {"UNKNOWN_TRANSACTION", "25000"},
    {"NOT_IN_TRANSACTION", "25000"},
    {"TRANSACTION_ALREADY_ABORTED", "25000"},
    {"AUTOCOMMIT_WRITE_CONFLICT", "25000"},
    {"READ_ONLY_VIOLATION", "25006"},
    {"INCOMPATIBLE_CLIENT", "08004"},
    {"SUBQUERY_MULTIPLE_ROWS", "21000"},
    {"QUERY_REJECTED", "HY000"},
    {"QUERY_TEXT_TOO_LARGE", "HY000"},
    {"QUERY_HAS_TOO_MANY_STAGES", "HY000"},
    {"AMBIGUOUS_RETURN_TYPE", "42725"},
    {"AMBIGUOUS_FUNCTION_CALL", "42725"},
    {"COLUMN_TYPE_UNKNOWN", "42000"},
    {"NESTED_AGGREGATION", "42000"},
    {"NESTED_WINDOW", "42000"},
    {"NESTED_ROW_PATTERN_RECOGNITION", "42000"},
    {"TOO_MANY_ARGUMENTS", "42000"},
    {"TOO_MANY_GROUPING_SETS", "42000"},
    {"NULL_TREATMENT_NOT_ALLOWED", "42000"},
    {"VIEW_IS_STALE", "42000"},
    {"VIEW_IS_RECURSIVE", "42000"},
    {"EXPRESSION_NOT_CONSTANT", "42000"},
    {"EXPRESSION_NOT_AGGREGATE", "42000"},
    {"EXPRESSION_NOT_SCALAR", "42000"},
    {"EXPRESSION_NOT_IN_DISTINCT", "42000"},
    {"COLUMN_MASK_FAILURE", "42000"},
    {"ROW_FILTER_FAILURE", "42000"},
    {"NAME_TOO_LONG", "42000"},
    {"CATALOG_UNAVAILABLE", "08001"},
    {"CATALOG_STORE_ERROR", "HY000"},
    {"SECURITY_ERROR", "42501"},
    // --- resource errors (INSUFFICIENT_RESOURCES); EXCEEDED_* handled by the prefix rule too
    {"EXCEEDED_GLOBAL_MEMORY_LIMIT", "57014"},
    {"EXCEEDED_LOCAL_MEMORY_LIMIT", "57014"},
    {"EXCEEDED_CPU_LIMIT", "57014"},
    {"EXCEEDED_SPILL_LIMIT", "57014"},
    {"EXCEEDED_SCAN_LIMIT", "57014"},
    {"EXCEEDED_FUNCTION_MEMORY_LIMIT", "57014"},
    {"EXCEEDED_TASK_DESCRIPTOR_STORAGE_CAPACITY", "57014"},
    {"CLUSTER_OUT_OF_MEMORY", "HY001"},
    {"INSUFFICIENT_RESOURCES", "HY001"},
    // --- internal / transport-ish errors
    {"INTERNAL_ERROR", "HY000"},
    {"GENERIC_INTERNAL_ERROR", "HY000"},
    {"TOO_MANY_REQUESTS_FAILED", "08S01"},
    {"PAGE_TRANSPORT_ERROR", "08S01"},
    {"PAGE_TRANSPORT_TIMEOUT", "HYT00"},
    {"NO_NODES_AVAILABLE", "08001"},
    {"REMOTE_HOST_GONE", "08S01"},
    {"REMOTE_TASK_ERROR", "HY000"},
    {"REMOTE_TASK_MISMATCH", "HY000"},
    {"REMOTE_BUFFER_CLOSE_FAILED", "HY000"},
    {"SERVER_STARTING_UP", "08001"},
    {"SERVER_SHUTTING_DOWN", "08S01"},
    {"STACK_OVERFLOW", "HY001"},
    {"CONFIGURATION_INVALID", "HY000"},
    {"CONFIGURATION_UNAVAILABLE", "HY000"},
    {"EXTERNAL", "HY000"},
};

/// Recognises the leading SQL keyword(s) of `sql` after skipping whitespace and comments.
/// Returns the uppercase word and advances `pos` past it.
std::string nextWord(std::string_view sql, size_t& pos) {
  // Skip whitespace, `-- ...` line comments and `/* ... */` block comments.
  for (;;) {
    while (pos < sql.size() && (sql[pos] == ' ' || sql[pos] == '\t' || sql[pos] == '\r' || sql[pos] == '\n' ||
                                sql[pos] == '\f' || sql[pos] == '\v')) {
      ++pos;
    }
    if (pos + 1 < sql.size() && sql[pos] == '-' && sql[pos + 1] == '-') {
      while (pos < sql.size() && sql[pos] != '\n') ++pos;
      continue;
    }
    if (pos + 1 < sql.size() && sql[pos] == '/' && sql[pos + 1] == '*') {
      const size_t end = sql.find("*/", pos + 2);
      pos = end == std::string_view::npos ? sql.size() : end + 2;
      continue;
    }
    break;
  }
  // A statement may start with '(' (parenthesised query) — treat as a query.
  if (pos < sql.size() && sql[pos] == '(') return "(";
  const size_t start = pos;
  while (pos < sql.size()) {
    const char c = sql[pos];
    const bool wordChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!wordChar) break;
    ++pos;
  }
  return toUpper(sql.substr(start, pos - start));
}

/// Sort key of a status record: (row group, row number, severity). Row groups: 0 = row number
/// unknown, 1 = no row, 2 = a specific row.
struct DiagOrderKey {
  int group = 1;
  SQLLEN row = 0;
  int severity = 0;  // 0 = error, 1 = warning
  bool operator<(const DiagOrderKey& o) const {
    // ODBC "Sequence of Status Records": errors outrank warnings; among records of the same class
    // the row rule applies (row unknown, then no row, then ascending row numbers).
    if (severity != o.severity) return severity < o.severity;
    if (group != o.group) return group < o.group;
    return row < o.row;
  }
};

DiagOrderKey orderKeyOf(const DiagRecord& r) {
  DiagOrderKey k;
  if (r.rowNumber == SQL_ROW_NUMBER_UNKNOWN) {
    k.group = 0;
  } else if (r.rowNumber > 0) {
    k.group = 2;
    k.row = r.rowNumber;
  }
  k.severity = r.isWarning() ? 1 : 0;
  return k;
}

}  // namespace

// ---- Diag -------------------------------------------------------------------------------------

void Diag::clear() {
  records_.clear();
  returnCode = SQL_SUCCESS;
  rowCount = -1;
  cursorRowCount = -1;
  dynamicFunction.clear();
  dynamicFunctionCode = SQL_DIAG_UNKNOWN_STATEMENT;
}

const DiagRecord* Diag::record(size_t index1) const {
  if (index1 < 1 || index1 > records_.size()) return nullptr;
  return &records_[index1 - 1];
}

SQLRETURN Diag::add(DiagRecord rec) {
  if (rec.serverName.empty()) rec.serverName = serverName;
  if (rec.connectionName.empty()) rec.connectionName = connectionName;
  // A SQLSTATE must always be five characters: pad/trim defensively so SQLGetDiagRec never emits
  // a malformed state to the application.
  if (rec.sqlstate.size() != 5) {
    if (rec.sqlstate.empty()) rec.sqlstate = sqlstate::kGeneralError;
    rec.sqlstate.resize(5, '0');
  }
  const bool warning = rec.isWarning();
  // ODBC "Sequence of Status Records": errors precede warnings; within one class, records whose
  // row is unknown come first, then records that belong to no row, then the rows in ascending
  // SQL_DIAG_ROW_NUMBER order. Equal keys keep insertion order (stable insert).
  const DiagOrderKey key = orderKeyOf(rec);
  auto pos = std::find_if(records_.begin(), records_.end(),
                          [&key](const DiagRecord& r) { return key < orderKeyOf(r); });
  records_.insert(pos, std::move(rec));
  return warning ? SQL_SUCCESS_WITH_INFO : SQL_ERROR;
}

SQLRETURN Diag::addError(std::string_view sqlstate, std::string_view message, SQLINTEGER native, SQLLEN row,
                         SQLINTEGER column) {
  DiagRecord rec;
  rec.sqlstate.assign(sqlstate.data(), sqlstate.size());
  rec.native = native;
  rec.message = formatDriverMessage(message);
  rec.rowNumber = row;
  rec.columnNumber = column;
  return add(std::move(rec));
}

SQLRETURN Diag::addWarning(std::string_view sqlstate, std::string_view message, SQLINTEGER native, SQLLEN row,
                           SQLINTEGER column) {
  // Same path as addError: the class of the SQLSTATE decides the return code and the ordering.
  return addError(sqlstate, message, native, row, column);
}

SQLRETURN Diag::addServerError(const TrinoQueryError& e) {
  DiagRecord rec;
  rec.sqlstate = e.sqlstate().empty() ? sqlstateForTrinoError(e.errorName(), e.errorType()) : e.sqlstate();
  rec.native = e.native();
  rec.message = formatServerMessage(e.message());
  rec.rowNumber = e.rowNumber();
  rec.columnNumber = e.columnNumber();
  return add(std::move(rec));
}

SQLRETURN Diag::addException(const DriverError& e) {
  DiagRecord rec;
  rec.sqlstate = e.sqlstate().empty() ? std::string(sqlstate::kGeneralError) : e.sqlstate();
  rec.native = e.native();
  rec.message = formatDriverMessage(e.message());
  rec.rowNumber = e.rowNumber();
  rec.columnNumber = e.columnNumber();
  return add(std::move(rec));
}

SQLRETURN Diag::addException(const std::exception& e) {
  const char* what = e.what();
  return addError(sqlstate::kGeneralError, what ? std::string_view(what) : std::string_view("Unknown error"));
}

std::string Diag::formatDriverMessage(std::string_view text) {
  std::string out;
  out.reserve(std::strlen(kDriverPrefix) + text.size());
  out += kDriverPrefix;
  out.append(text.data(), text.size());
  return out;
}

std::string Diag::formatServerMessage(std::string_view text) {
  std::string out;
  out.reserve(std::strlen(kServerPrefix) + text.size());
  out += kServerPrefix;
  out.append(text.data(), text.size());
  return out;
}

const char* Diag::sqlstateForTrinoError(std::string_view errorName, std::string_view errorType) {
  const std::string name = toUpper(trim(errorName));
  const std::string type = toUpper(trim(errorType));
  for (const auto& entry : kErrorNames) {
    if (name == entry.name) return entry.sqlstate;
  }
  // Prefix rules from ARCHITECTURE.md §8.
  if (startsWith(name, "EXCEEDED_")) return "57014";
  if (startsWith(name, "MISSING_") || startsWith(name, "INVALID_")) {
    // These are user errors by construction; only an explicit non-user type overrides the mapping.
    if (type.empty() || type == "USER_ERROR") return "42000";
  }
  if (type == "INSUFFICIENT_RESOURCES") return "HY001";
  return "HY000";  // INTERNAL_ERROR, EXTERNAL, unknown user errors
}

void Diag::classifyDynamicFunction(std::string_view sql, std::string& name, SQLINTEGER& code) {
  name.clear();
  code = SQL_DIAG_UNKNOWN_STATEMENT;
  size_t pos = 0;
  const std::string first = nextWord(sql, pos);
  if (first.empty()) return;

  auto set = [&](const char* n, SQLINTEGER c) {
    name = n;
    code = c;
  };

  // Statements producing a result set.
  if (first == "SELECT" || first == "WITH" || first == "VALUES" || first == "TABLE" || first == "SHOW" ||
      first == "DESCRIBE" || first == "DESC" || first == "EXPLAIN" || first == "(") {
    set("SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    return;
  }
  if (first == "INSERT") { set("INSERT", SQL_DIAG_INSERT); return; }
  if (first == "UPDATE") { set("UPDATE WHERE", SQL_DIAG_UPDATE_WHERE); return; }
  if (first == "DELETE") { set("DELETE WHERE", SQL_DIAG_DELETE_WHERE); return; }
  if (first == "MERGE") { set("MERGE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "CALL") { set("CALL", SQL_DIAG_CALL); return; }
  if (first == "GRANT") { set("GRANT", SQL_DIAG_GRANT); return; }
  if (first == "REVOKE") { set("REVOKE", SQL_DIAG_REVOKE); return; }

  if (first == "CREATE" || first == "DROP" || first == "ALTER") {
    // Skip optional modifiers ("OR REPLACE", "MATERIALIZED", "IF EXISTS" comes after the object).
    std::string second = nextWord(sql, pos);
    bool materialized = false;
    while (second == "OR" || second == "REPLACE" || second == "TEMPORARY" || second == "TEMP") {
      second = nextWord(sql, pos);
    }
    if (second == "MATERIALIZED") {
      materialized = true;
      second = nextWord(sql, pos);
    }
    const std::string object = materialized ? "MATERIALIZED " + second : second;
    name = first + (object.empty() ? "" : " " + object);
    if (first == "CREATE") {
      if (second == "TABLE") code = SQL_DIAG_CREATE_TABLE;
      else if (second == "VIEW" && !materialized) code = SQL_DIAG_CREATE_VIEW;
      else if (second == "SCHEMA") code = SQL_DIAG_CREATE_SCHEMA;
      else if (second == "INDEX") code = SQL_DIAG_CREATE_INDEX;
    } else if (first == "DROP") {
      if (second == "TABLE") code = SQL_DIAG_DROP_TABLE;
      else if (second == "VIEW" && !materialized) code = SQL_DIAG_DROP_VIEW;
      else if (second == "SCHEMA") code = SQL_DIAG_DROP_SCHEMA;
      else if (second == "INDEX") code = SQL_DIAG_DROP_INDEX;
    } else {  // ALTER
      if (second == "TABLE") code = SQL_DIAG_ALTER_TABLE;
    }
    return;
  }

  if (first == "START" || first == "BEGIN") { set("START TRANSACTION", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "COMMIT") { set("COMMIT", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "ROLLBACK") { set("ROLLBACK", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "USE") { set("USE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "SET" || first == "RESET") {
    const std::string second = nextWord(sql, pos);
    name = first + (second.empty() ? "" : " " + second);
    return;
  }
  if (first == "PREPARE") { set("PREPARE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "DEALLOCATE") { set("DEALLOCATE PREPARE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "EXECUTE") {
    // `EXECUTE IMMEDIATE '<sql>'` wraps the real statement; the caller cannot see through the
    // literal here, so report the outer form.
    const std::string second = nextWord(sql, pos);
    name = second == "IMMEDIATE" ? "EXECUTE IMMEDIATE" : "EXECUTE";
    return;
  }
  if (first == "TRUNCATE") { set("TRUNCATE TABLE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "ANALYZE") { set("ANALYZE", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "COMMENT") { set("COMMENT", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "REFRESH") { set("REFRESH MATERIALIZED VIEW", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  if (first == "DENY") { set("DENY", SQL_DIAG_UNKNOWN_STATEMENT); return; }
  // Anything else: expose the leading keyword so SQLGetDiagField still returns something useful.
  name = first;
}

SQLRETURN mergeReturnCode(SQLRETURN a, SQLRETURN b) {
  return returnCodeRank(b) > returnCodeRank(a) ? b : a;
}

}  // namespace trino_odbc
