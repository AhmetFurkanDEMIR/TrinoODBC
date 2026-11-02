#pragma once
// ODBC escape-sequence translation and SQL text helpers.
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {

/// Translates {d}, {t}, {ts}, {interval}, {fn}, {oj}, {escape}, {call}, {? = call} into Trino SQL.
/// Sequences inside string literals, quoted identifiers and comments are left untouched.
/// Unknown {fn} names are passed through as plain function calls with a warning appended to
/// `warnings` (may be null). Throws DriverError(42000) on malformed braces.
std::string translateOdbcEscapes(std::string_view sql, std::vector<std::string>* warnings = nullptr);

/// Number of `?` parameter markers outside literals/identifiers/comments.
size_t countParameterMarkers(std::string_view sql);
/// Splits on parameter markers -> n+1 pieces (pieces concatenated with the rendered literals rebuild the SQL).
std::vector<std::string_view> splitOnParameterMarkers(std::string_view sql);

struct SqlKind {
  bool returnsRows = false;   // SELECT/SHOW/DESCRIBE/EXPLAIN/VALUES/WITH/TABLE ...
  bool isDml = false;         // INSERT/UPDATE/DELETE/MERGE
  bool isDdl = false;         // CREATE/DROP/ALTER/...
  bool isTransaction = false; // START TRANSACTION/COMMIT/ROLLBACK
  bool isUse = false;         // USE catalog[.schema]
  bool isSet = false;         // SET SESSION / SET ROLE / RESET SESSION
  bool isPrepare = false;     // PREPARE / DEALLOCATE PREPARE / EXECUTE
  std::string firstKeyword;   // upper-case
};
/// Cheap classification of a statement from its leading keywords.
SqlKind classifySql(std::string_view sql);

/// First keyword of the statement (upper-case), skipping comments and whitespace.
std::string firstKeyword(std::string_view sql);

}  // namespace trino_odbc
