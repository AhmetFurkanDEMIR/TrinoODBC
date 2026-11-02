#pragma once
// Catalog functions (SQLTables, SQLColumns, SQLGetTypeInfo, SQLPrimaryKeys, ...). Each returns an
// in-memory result set laid out exactly as the ODBC specification mandates.
#include "core/handles.h"
#include "result/row_source.h"

#include <memory>
#include <optional>
#include <string>

namespace trino_odbc {

/// Pattern / identifier arguments of a catalog call (nullopt = argument was a null pointer).
struct CatalogArgs {
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  std::optional<std::string> column;
  std::optional<std::string> tableTypes;   // SQLTables TableType list ("TABLE,VIEW" / "'TABLE'")
  /// True when SQL_ATTR_METADATA_ID is SQL_TRUE or MetadataCaseSensitive=1: identifiers are literals.
  bool identifiersAreLiterals = false;
};

std::unique_ptr<MemoryRowSource> catalogTables(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogColumns(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogTypeInfo(Connection& conn, SQLSMALLINT dataType);
std::unique_ptr<MemoryRowSource> catalogPrimaryKeys(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogForeignKeys(Connection& conn, const CatalogArgs& pk, const CatalogArgs& fk);
std::unique_ptr<MemoryRowSource> catalogStatistics(Connection& conn, const CatalogArgs& args, SQLUSMALLINT unique, SQLUSMALLINT reserved);
std::unique_ptr<MemoryRowSource> catalogSpecialColumns(Connection& conn, SQLUSMALLINT idType, const CatalogArgs& args, SQLUSMALLINT scope, SQLUSMALLINT nullable);
std::unique_ptr<MemoryRowSource> catalogProcedures(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogProcedureColumns(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogTablePrivileges(Connection& conn, const CatalogArgs& args);
std::unique_ptr<MemoryRowSource> catalogColumnPrivileges(Connection& conn, const CatalogArgs& args);

/// Builds a `WHERE` predicate fragment for an ODBC search pattern against `columnExpr`
/// (`LIKE ... ESCAPE '\'` or `=` when literal). Returns empty string when `pattern` is nullopt or "%"
/// (match all). Exposed for tests.
std::string patternPredicate(std::string_view columnExpr, const std::optional<std::string>& pattern, bool literal);

}  // namespace trino_odbc
