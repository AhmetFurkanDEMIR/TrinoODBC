#pragma once
// Module-private helpers of the catalog package: the SQL text sent to `system.jdbc.*` for every
// catalog function, parsing of the SQLTables TableType list, the SQL_ALL_* special-enumeration
// detection and the identifier normalisation used when SQL_ATTR_METADATA_ID is on.
//
// Nothing here touches a Connection, so tests/unit/test_catalog_sql.cpp can pin the exact SQL the
// driver sends without a server. catalog.cpp runs the queries and reshapes the rows.
#include "catalog/catalog.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trino_odbc {
namespace catalog_sql {

/// The SQLTables argument combinations the ODBC specification singles out, plus one lenient
/// extension (CatalogSchemas) that turns an otherwise empty result into something useful.
enum class SpecialEnumeration {
  None,            // ordinary SQLTables query
  AllCatalogs,     // CatalogName = SQL_ALL_CATALOGS ("%"), SchemaName = "", TableName = ""
  AllSchemas,      // SchemaName = SQL_ALL_SCHEMAS ("%"), CatalogName = "", TableName = ""
  CatalogSchemas,  // SchemaName = "%", TableName = "", CatalogName null or a name/pattern: schemas of those catalogs
  AllTableTypes    // TableType = SQL_ALL_TABLE_TYPES ("%"), CatalogName = SchemaName = TableName = ""
};

SpecialEnumeration detectSpecialEnumeration(const CatalogArgs& args);

/// Parses the SQLTables TableType value list: `TABLE,VIEW`, `'TABLE','VIEW'`, `SYSTEM TABLE`,
/// mixed case and surrounding whitespace. Values are upper-cased, internal whitespace is
/// collapsed, duplicates dropped. Returns an empty list (= no filter) for a null pointer, an empty
/// string, `%`, or a list that contains `%`.
std::vector<std::string> parseTableTypes(const std::optional<std::string>& tableTypes);

/// Applies the SQL_ATTR_METADATA_ID identifier rules to an argument: a delimited identifier
/// (`"na""me"`) loses its quotes and doubled quotes; anything else loses trailing blanks only.
/// Case is never folded because Trino stores identifiers in lower case as written by the user.
std::string normalizeIdentifierArgument(std::string_view value);

/// Predicate for an ordinary (non-pattern) argument such as the CatalogName of SQLColumns:
/// `columnExpr = 'value'`; empty string when the argument is a null pointer.
std::string ordinaryPredicate(std::string_view columnExpr, const std::optional<std::string>& value, bool literal);

/// `table_type IN ('TABLE', 'VIEW')` or empty when `types` is empty.
std::string tableTypesPredicate(const std::vector<std::string>& types);

/// " WHERE a AND b" from the non-empty fragments; empty when there are none.
std::string whereClause(const std::vector<std::string>& predicates);

// ---- query text ------------------------------------------------------------------------------
// The column lists and ORDER BY clauses mirror the Trino JDBC driver so both drivers behave the
// same against any Trino version (system.jdbc.* is the JDBC metadata contract).

std::string buildCatalogsQuery();
std::string buildSchemasQuery(const CatalogArgs& args);
std::string buildTableTypesQuery();
std::string buildTablesQuery(const CatalogArgs& args);
std::string buildColumnsQuery(const CatalogArgs& args);
std::string buildProceduresQuery(const CatalogArgs& args);
std::string buildProcedureColumnsQuery(const CatalogArgs& args);

}  // namespace catalog_sql
}  // namespace trino_odbc
