// ANSI catalog entry points (SQLTables, SQLColumns, SQLPrimaryKeys, SQLForeignKeys, SQLStatistics,
// SQLSpecialColumns, SQLProcedures, SQLProcedureColumns, SQLTablePrivileges, SQLColumnPrivileges,
// SQLGetTypeInfo) and the shared impl:: bodies used by their *W twins in api_unicode.cpp.
//
// The entry points only convert text arguments; the impl:: functions perform the ODBC state checks,
// run the catalog query (src/catalog) while the previous statement result is still intact — so a
// failing call leaves the statement exactly as it was, as the spec's state tables require — and
// finally install the in-memory result as the statement's cursor.
#include "odbc/api_common.h"

#include "catalog/catalog.h"
#include "core/client_cancel.h"
#include "result/result_set.h"
#include "types/odbc_types.h"
#include "util/errors.h"
#include "util/log.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace trino_odbc {

namespace {

constexpr const char* kLog = "odbc";

/// Identifier arguments are literals (no pattern matching) when the application asked for it via
/// SQL_ATTR_METADATA_ID or the DSN forces it with MetadataCaseSensitive=1.
bool identifiersAreLiterals(const Statement& s) {
  return s.metadataId == SQL_TRUE || (s.conn != nullptr && s.conn->config.metadataCaseSensitive);
}

/// Converts one narrow catalog argument (null pointer -> nullopt, kept distinct from "").
std::optional<std::string> narrowArg(const Statement& s, const SQLCHAR* text, SQLSMALLINT length) {
  return textIn(text, static_cast<SQLLEN>(length), ansiEncodingOf(s));
}

/// Builds the CatalogArgs of an ANSI call.
CatalogArgs makeArgs(const Statement& s, const SQLCHAR* catalog, SQLSMALLINT catalogLen, const SQLCHAR* schema,
                     SQLSMALLINT schemaLen, const SQLCHAR* table, SQLSMALLINT tableLen,
                     const SQLCHAR* column = nullptr, SQLSMALLINT columnLen = 0, const SQLCHAR* tableTypes = nullptr,
                     SQLSMALLINT tableTypesLen = 0) {
  CatalogArgs args;
  args.catalog = narrowArg(s, catalog, catalogLen);
  args.schema = narrowArg(s, schema, schemaLen);
  args.table = narrowArg(s, table, tableLen);
  args.column = narrowArg(s, column, columnLen);
  args.tableTypes = narrowArg(s, tableTypes, tableTypesLen);
  args.identifiersAreLiterals = identifiersAreLiterals(s);
  return args;
}

/// State checks shared by every catalog function: the connection must be live, the statement must
/// not be waiting for data-at-execution or executing, and no cursor may be open (24000) — an
/// application must close its cursor before reusing the statement.
void checkCatalogState(const Statement& s) {
  if (s.conn == nullptr) throw DriverError(sqlstate::kNotConnected, "statement has no connection");
  requireConnected(*s.conn);
  requireNotBusy(s);
  requireNoCursor(s);
}

/// HY009 for a null argument. Used for the arguments the spec declares mandatory (ordinary
/// arguments such as SQLPrimaryKeys' TableName) and, when SQL_ATTR_METADATA_ID is SQL_TRUE, for
/// every identifier argument (they are then identifiers, and identifiers cannot be omitted).
void requireArg(const std::optional<std::string>& value, const char* function, const char* argName) {
  if (!value) {
    throw DriverError(sqlstate::kInvalidNullPointer,
                      std::string(function) + ": the " + argName + " argument must not be a null pointer");
  }
}

void requireArgIfMetadataId(const Statement& s, const std::optional<std::string>& value, const char* function,
                            const char* argName) {
  if (s.metadataId == SQL_TRUE) requireArg(value, function, argName);
}

/// Common tail: the identifier arguments that become mandatory under SQL_ATTR_METADATA_ID.
void requireTableIdentifiers(const Statement& s, const CatalogArgs& args, const char* function) {
  requireArgIfMetadataId(s, args.catalog, function, "CatalogName");
  requireArgIfMetadataId(s, args.schema, function, "SchemaName");
  requireArgIfMetadataId(s, args.table, function, "TableName");
}

/// Surfaces warnings the catalog query produced (Trino warnings on the metadata query) as 01000
/// records so the application sees SQL_SUCCESS_WITH_INFO.
SQLRETURN addSourceWarnings(Statement& s, const RowSource& source) {
  SQLRETURN rc = SQL_SUCCESS;
  for (const QueryWarning& w : source.warnings()) {
    std::string text = "[Trino] ";
    if (!w.name.empty()) {
      text += w.name;
      text += ": ";
    }
    text += w.message;
    rc = mergeReturnCode(rc, s.diag.addWarning(sqlstate::kGeneralWarning, text, w.code));
  }
  return rc;
}

/// Runs one catalog function with the statement's SQLCancel flag visible to the metadata queries it
/// starts: the catalog entry points only receive the Connection (see core/client_cancel.h).
template <typename Fn>
std::unique_ptr<MemoryRowSource> runCancellable(Statement& s, Fn&& fn) {
  MetadataCancelScope scope(&s.cancelRequested);
  return fn();
}

}  // namespace

namespace impl {

SQLRETURN installCatalogResult(Statement& s, std::unique_ptr<MemoryRowSource> rows, const char* dynamicFunction) {
  if (!rows) {
    throw DriverError(sqlstate::kGeneralError, std::string(dynamicFunction) + ": the catalog query produced no result");
  }
  // A SQLCancel that arrived while the catalog query ran (or after it finished but before the
  // call returns) is reported, not silently dropped: the previous result stays untouched.
  if (s.cancelRequested.load()) throw CancelledError();
  const size_t totalRows = rows->totalRows();

  // Discard whatever the statement held before: a previous DML result (row count) and the prepared
  // statement text. Per the ODBC state tables a cursor produced by a catalog function returns the
  // statement to S1 (Allocated) when closed, exactly like SQLExecDirect, not to the prepared state.
  s.closeCursor();
  s.resetPrepared();

  auto result = std::make_unique<ResultSet>(std::move(rows));
  if (s.maxRows > 0) result->setMaxRows(static_cast<int64_t>(s.maxRows));
  s.result = std::move(result);
  s.rebuildIrd(s.result->columns());
  s.getData.reset(s.result->columnCount());
  s.rowsetPos = 1;
  s.rowNumber = 0;
  s.rowCount = -1;
  s.queryId.clear();
  s.state = StmtState::CursorOpen;

  s.diag.rowCount = -1;
  s.diag.cursorRowCount = static_cast<SQLLEN>(totalRows);
  s.diag.dynamicFunction = dynamicFunction;
  s.diag.dynamicFunctionCode = SQL_DIAG_SELECT_CURSOR;

  TRINO_LOG_DEBUG(kLog, "%s: result installed with %zu row(s), %zu column(s)", dynamicFunction, totalRows,
                  s.result->columnCount());
  return addSourceWarnings(s, s.result->source());
}

SQLRETURN tables(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  requireTableIdentifiers(s, args, "SQLTables");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogTables(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLTables");
}

SQLRETURN columns(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  requireTableIdentifiers(s, args, "SQLColumns");
  requireArgIfMetadataId(s, args.column, "SQLColumns", "ColumnName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogColumns(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLColumns");
}

SQLRETURN primaryKeys(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  // TableName is an ordinary argument here, so it is mandatory regardless of SQL_ATTR_METADATA_ID.
  requireArg(args.table, "SQLPrimaryKeys", "TableName");
  requireArgIfMetadataId(s, args.catalog, "SQLPrimaryKeys", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLPrimaryKeys", "SchemaName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogPrimaryKeys(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLPrimaryKeys");
}

SQLRETURN foreignKeys(Statement& s, CatalogArgs pk, CatalogArgs fk) {
  checkCatalogState(s);
  if (!pk.table && !fk.table) {
    throw DriverError(sqlstate::kInvalidNullPointer,
                      "SQLForeignKeys: PKTableName and FKTableName must not both be null pointers");
  }
  requireArgIfMetadataId(s, pk.catalog, "SQLForeignKeys", "PKCatalogName");
  requireArgIfMetadataId(s, pk.schema, "SQLForeignKeys", "PKSchemaName");
  requireArgIfMetadataId(s, fk.catalog, "SQLForeignKeys", "FKCatalogName");
  requireArgIfMetadataId(s, fk.schema, "SQLForeignKeys", "FKSchemaName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogForeignKeys(*s.conn, pk, fk); });
  return installCatalogResult(s, std::move(rows), "SQLForeignKeys");
}

SQLRETURN statistics(Statement& s, CatalogArgs args, SQLUSMALLINT unique, SQLUSMALLINT reserved) {
  checkCatalogState(s);
  requireArg(args.table, "SQLStatistics", "TableName");
  requireArgIfMetadataId(s, args.catalog, "SQLStatistics", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLStatistics", "SchemaName");
  // Option validation (HY100 / HY101) lives in catalogStatistics.
  std::unique_ptr<MemoryRowSource> rows =
      runCancellable(s, [&] { return catalogStatistics(*s.conn, args, unique, reserved); });
  return installCatalogResult(s, std::move(rows), "SQLStatistics");
}

SQLRETURN specialColumns(Statement& s, SQLUSMALLINT idType, CatalogArgs args, SQLUSMALLINT scope,
                         SQLUSMALLINT nullable) {
  checkCatalogState(s);
  requireArg(args.table, "SQLSpecialColumns", "TableName");
  requireArgIfMetadataId(s, args.catalog, "SQLSpecialColumns", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLSpecialColumns", "SchemaName");
  // HY097 / HY098 / HY099 are raised by catalogSpecialColumns.
  std::unique_ptr<MemoryRowSource> rows =
      runCancellable(s, [&] { return catalogSpecialColumns(*s.conn, idType, args, scope, nullable); });
  return installCatalogResult(s, std::move(rows), "SQLSpecialColumns");
}

SQLRETURN procedures(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  // The procedure name travels in CatalogArgs::table.
  requireArgIfMetadataId(s, args.catalog, "SQLProcedures", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLProcedures", "SchemaName");
  requireArgIfMetadataId(s, args.table, "SQLProcedures", "ProcName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogProcedures(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLProcedures");
}

SQLRETURN procedureColumns(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  requireArgIfMetadataId(s, args.catalog, "SQLProcedureColumns", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLProcedureColumns", "SchemaName");
  requireArgIfMetadataId(s, args.table, "SQLProcedureColumns", "ProcName");
  requireArgIfMetadataId(s, args.column, "SQLProcedureColumns", "ColumnName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogProcedureColumns(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLProcedureColumns");
}

SQLRETURN tablePrivileges(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  requireTableIdentifiers(s, args, "SQLTablePrivileges");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogTablePrivileges(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLTablePrivileges");
}

SQLRETURN columnPrivileges(Statement& s, CatalogArgs args) {
  checkCatalogState(s);
  requireArg(args.table, "SQLColumnPrivileges", "TableName");
  requireArgIfMetadataId(s, args.catalog, "SQLColumnPrivileges", "CatalogName");
  requireArgIfMetadataId(s, args.schema, "SQLColumnPrivileges", "SchemaName");
  requireArgIfMetadataId(s, args.column, "SQLColumnPrivileges", "ColumnName");
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogColumnPrivileges(*s.conn, args); });
  return installCatalogResult(s, std::move(rows), "SQLColumnPrivileges");
}

SQLRETURN getTypeInfo(Statement& s, SQLSMALLINT dataType) {
  checkCatalogState(s);
  // Validate before anything is torn down so a bad type leaves the statement untouched.
  if (dataType != SQL_ALL_TYPES && !isValidSqlType(dataType)) {
    throw DriverError(sqlstate::kInvalidSqlDataType,
                      "SQLGetTypeInfo: invalid SQL data type " + std::to_string(dataType));
  }
  std::unique_ptr<MemoryRowSource> rows = runCancellable(s, [&] { return catalogTypeInfo(*s.conn, dataType); });
  return installCatalogResult(s, std::move(rows), "SQLGetTypeInfo");
}

}  // namespace impl

}  // namespace trino_odbc

// ---- Exported ANSI entry points ----------------------------------------------------------------

using namespace trino_odbc;

extern "C" {

TRINO_ODBC_API SQLRETURN SQL_API SQLTables(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                           SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLCHAR* szTableName,
                                           SQLSMALLINT cbTableName, SQLCHAR* szTableType, SQLSMALLINT cbTableType) {
  return guardStmt(hstmt, "SQLTables", [&](Statement& s) -> SQLRETURN {
    return impl::tables(s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName,
                                    cbTableName, nullptr, 0, szTableType, cbTableType));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColumns(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                            SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLCHAR* szTableName,
                                            SQLSMALLINT cbTableName, SQLCHAR* szColumnName, SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLColumns", [&](Statement& s) -> SQLRETURN {
    return impl::columns(s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName,
                                     cbTableName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLCHAR* szTableName,
                                                SQLSMALLINT cbTableName) {
  return guardStmt(hstmt, "SQLPrimaryKeys", [&](Statement& s) -> SQLRETURN {
    return impl::primaryKeys(
        s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLForeignKeys(SQLHSTMT hstmt, SQLCHAR* szPkCatalogName, SQLSMALLINT cbPkCatalogName,
                                                SQLCHAR* szPkSchemaName, SQLSMALLINT cbPkSchemaName,
                                                SQLCHAR* szPkTableName, SQLSMALLINT cbPkTableName,
                                                SQLCHAR* szFkCatalogName, SQLSMALLINT cbFkCatalogName,
                                                SQLCHAR* szFkSchemaName, SQLSMALLINT cbFkSchemaName,
                                                SQLCHAR* szFkTableName, SQLSMALLINT cbFkTableName) {
  return guardStmt(hstmt, "SQLForeignKeys", [&](Statement& s) -> SQLRETURN {
    return impl::foreignKeys(
        s, makeArgs(s, szPkCatalogName, cbPkCatalogName, szPkSchemaName, cbPkSchemaName, szPkTableName, cbPkTableName),
        makeArgs(s, szFkCatalogName, cbFkCatalogName, szFkSchemaName, cbFkSchemaName, szFkTableName, cbFkTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLStatistics(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                               SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLCHAR* szTableName,
                                               SQLSMALLINT cbTableName, SQLUSMALLINT fUnique, SQLUSMALLINT fAccuracy) {
  return guardStmt(hstmt, "SQLStatistics", [&](Statement& s) -> SQLRETURN {
    return impl::statistics(
        s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName), fUnique,
        fAccuracy);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT hstmt, SQLUSMALLINT fColType, SQLCHAR* szCatalogName,
                                                   SQLSMALLINT cbCatalogName, SQLCHAR* szSchemaName,
                                                   SQLSMALLINT cbSchemaName, SQLCHAR* szTableName,
                                                   SQLSMALLINT cbTableName, SQLUSMALLINT fScope,
                                                   SQLUSMALLINT fNullable) {
  return guardStmt(hstmt, "SQLSpecialColumns", [&](Statement& s) -> SQLRETURN {
    return impl::specialColumns(
        s, fColType,
        makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName), fScope,
        fNullable);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLProcedures(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                               SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLCHAR* szProcName,
                                               SQLSMALLINT cbProcName) {
  return guardStmt(hstmt, "SQLProcedures", [&](Statement& s) -> SQLRETURN {
    return impl::procedures(
        s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szProcName, cbProcName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLProcedureColumns(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                     SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName,
                                                     SQLCHAR* szProcName, SQLSMALLINT cbProcName, SQLCHAR* szColumnName,
                                                     SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLProcedureColumns", [&](Statement& s) -> SQLRETURN {
    return impl::procedureColumns(s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szProcName,
                                              cbProcName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLTablePrivileges(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                    SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName,
                                                    SQLCHAR* szTableName, SQLSMALLINT cbTableName) {
  return guardStmt(hstmt, "SQLTablePrivileges", [&](Statement& s) -> SQLRETURN {
    return impl::tablePrivileges(
        s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColumnPrivileges(SQLHSTMT hstmt, SQLCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                     SQLCHAR* szSchemaName, SQLSMALLINT cbSchemaName,
                                                     SQLCHAR* szTableName, SQLSMALLINT cbTableName,
                                                     SQLCHAR* szColumnName, SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLColumnPrivileges", [&](Statement& s) -> SQLRETURN {
    return impl::columnPrivileges(s, makeArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName,
                                              szTableName, cbTableName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT hstmt, SQLSMALLINT fSqlType) {
  return guardStmt(hstmt, "SQLGetTypeInfo",
                   [&](Statement& s) -> SQLRETURN { return impl::getTypeInfo(s, fSqlType); });
}

}  // extern "C"
