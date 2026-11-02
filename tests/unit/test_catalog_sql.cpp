// Unit tests for the connection-free half of the catalog package: the SQL text sent to
// system.jdbc.* for every catalog function, ODBC search-pattern escaping, the SQL_ALL_* special
// enumerations of SQLTables, the TableType list parser, SQL_ATTR_METADATA_ID identifier
// normalisation and the ODBC result layouts of the functions that answer without a server (keys,
// statistics, special columns, privileges).
//
// Nothing here opens a connection. The entry points that would run a query are exercised only to
// prove that they refuse with 08003 on an unconnected handle instead of touching the network.
#include "unit/test_framework.h"

#include "catalog/catalog.h"
#include "catalog/catalog_sql.h"
#include "core/handles.h"
#include "result/row_source.h"
#include "types/trino_type.h"
#include "util/errors.h"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using namespace trino_odbc;
using namespace trino_odbc::catalog_sql;

// Both sides spelled as std::string so string literals, raw strings and std::string mix freely.
#define CHECK_STR(a, b) CHECK_EQ(std::string(a), std::string(b))

namespace {

using OptStr = std::optional<std::string>;

OptStr S(const char* s) { return std::string(s); }

CatalogArgs makeArgs(OptStr catalog, OptStr schema, OptStr table, OptStr column = std::nullopt,
                     OptStr tableTypes = std::nullopt, bool literals = false) {
  CatalogArgs a;
  a.catalog = std::move(catalog);
  a.schema = std::move(schema);
  a.table = std::move(table);
  a.column = std::move(column);
  a.tableTypes = std::move(tableTypes);
  a.identifiersAreLiterals = literals;
  return a;
}

std::string joined(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.push_back('|');
    out.append(parts[i]);
  }
  return out;
}

std::string types(const OptStr& list) { return joined(parseTableTypes(list)); }

const char* name(SpecialEnumeration e) {
  switch (e) {
    case SpecialEnumeration::None: return "None";
    case SpecialEnumeration::AllCatalogs: return "AllCatalogs";
    case SpecialEnumeration::AllSchemas: return "AllSchemas";
    case SpecialEnumeration::CatalogSchemas: return "CatalogSchemas";
    case SpecialEnumeration::AllTableTypes: return "AllTableTypes";
  }
  return "?";
}

std::string detect(OptStr catalog, OptStr schema, OptStr table, OptStr tableTypes = std::nullopt,
                   bool literals = false) {
  return name(detectSpecialEnumeration(
      makeArgs(std::move(catalog), std::move(schema), std::move(table), std::nullopt, std::move(tableTypes), literals)));
}

/// SQLSTATE of the DriverError `fn` throws, or "<none>" when it returns normally.
std::string thrownSqlstate(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const DriverError& e) {
    return e.sqlstate();
  }
  return "<none>";
}

void checkColumnNames(const std::vector<ResultColumn>& cols, std::initializer_list<const char*> names) {
  REQUIRE_EQ(cols.size(), names.size());
  size_t i = 0;
  for (const char* expected : names) {
    CHECK_STR(cols[i].name, expected);
    ++i;
  }
}

void checkEmpty(MemoryRowSource& src) {
  CHECK_EQ(src.totalRows(), size_t{0});
  CHECK(src.nextPage() == nullptr);
  CHECK(src.nextPage() == nullptr);  // stays at end of data
  CHECK(src.warnings().empty());
}

// The fixed parts of every builder, so the WHERE clause is the only thing a test spells out.
constexpr const char* kSchemasSelect = "SELECT table_schem, table_catalog FROM system.jdbc.schemas";
constexpr const char* kSchemasOrder = " ORDER BY table_catalog, table_schem";
constexpr const char* kTablesSelect =
    "SELECT table_cat, table_schem, table_name, table_type, remarks, type_cat, type_schem, type_name, "
    "self_referencing_col_name, ref_generation FROM system.jdbc.tables";
constexpr const char* kTablesOrder = " ORDER BY table_type, table_cat, table_schem, table_name";
constexpr const char* kColumnsSelect =
    "SELECT table_cat, table_schem, table_name, column_name, data_type, type_name, column_size, buffer_length, "
    "decimal_digits, num_prec_radix, nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
    "char_octet_length, ordinal_position, is_nullable FROM system.jdbc.columns";
constexpr const char* kColumnsOrder = " ORDER BY table_cat, table_schem, table_name, ordinal_position";
constexpr const char* kProceduresSelect =
    "SELECT procedure_cat, procedure_schem, procedure_name, remarks, procedure_type, specific_name "
    "FROM system.jdbc.procedures";
constexpr const char* kProceduresOrder = " ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name";
constexpr const char* kProcColumnsSelect =
    "SELECT procedure_cat, procedure_schem, procedure_name, column_name, column_type, data_type, type_name, "
    "precision, length, scale, radix, nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
    "char_octet_length, ordinal_position, is_nullable, specific_name FROM system.jdbc.procedure_columns";
constexpr const char* kProcColumnsOrder =
    " ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name, ordinal_position";

std::string query(const char* select, const std::string& where, const char* order) {
  return std::string(select) + where + order;
}

}  // namespace

// ==================================================================================================
// patternPredicate
// ==================================================================================================

TEST_CASE("patternPredicate: a null pointer argument is not a filter") {
  CHECK_STR(patternPredicate("table_name", std::nullopt, false), "");
  CHECK_STR(patternPredicate("table_name", std::nullopt, true), "");
}

TEST_CASE("patternPredicate: '%' alone matches everything and produces no predicate") {
  CHECK_STR(patternPredicate("table_name", S("%"), false), "");
  // Only the bare "%" is special; anything longer is a real pattern.
  CHECK_STR(patternPredicate("table_name", S("%%"), false), R"(table_name LIKE '%%' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(" %"), false), R"(table_name LIKE ' %' ESCAPE '\')");
}

TEST_CASE("patternPredicate: empty string selects objects without that qualifier (matches nothing)") {
  CHECK_STR(patternPredicate("table_schem", S(""), false), "table_schem = ''");
  CHECK_STR(patternPredicate("table_schem", S(""), true), "table_schem = ''");
}

TEST_CASE("patternPredicate: names and wildcards are passed through to LIKE with the backslash escape") {
  CHECK_STR(patternPredicate("table_name", S("orders"), false), R"(table_name LIKE 'orders' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("ord%"), false), R"(table_name LIKE 'ord%' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("_rders"), false), R"(table_name LIKE '_rders' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("%_%"), false), R"(table_name LIKE '%_%' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("o_d%s"), false), R"(table_name LIKE 'o_d%s' ESCAPE '\')");
}

TEST_CASE("patternPredicate: ODBC escapes \\_ \\% and \\\\ are preserved for Trino's ESCAPE clause") {
  CHECK_STR(patternPredicate("table_name", S(R"(my\_table)"), false), R"(table_name LIKE 'my\_table' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(R"(100\%)"), false), R"(table_name LIKE '100\%' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(R"(a\\b)"), false), R"(table_name LIKE 'a\\b' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(R"(\_\%\\)"), false), R"(table_name LIKE '\_\%\\' ESCAPE '\')");
  // An escaped underscore next to a live wildcard: only the escaped one is literal.
  CHECK_STR(patternPredicate("column_name", S(R"(id\_%)"), false), R"(column_name LIKE 'id\_%' ESCAPE '\')");
}

TEST_CASE("patternPredicate: a backslash that escapes nothing is doubled so Trino sees a literal backslash") {
  // Trino rejects a dangling escape character outright; doubling turns it into "no match" instead.
  CHECK_STR(patternPredicate("table_name", S(R"(C:\temp)"), false), R"(table_name LIKE 'C:\\temp' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(R"(abc\)"), false), R"(table_name LIKE 'abc\\' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S(R"(\)"), false), R"(table_name LIKE '\\' ESCAPE '\')");
  // Three backslashes: an escaped pair followed by a dangling one.
  CHECK_STR(patternPredicate("table_name", S(R"(\\\)"), false), R"(table_name LIKE '\\\\' ESCAPE '\')");
  // A backslash before a quote escapes nothing; the quote itself is doubled as usual.
  CHECK_STR(patternPredicate("table_name", S(R"(a\'b)"), false), R"(table_name LIKE 'a\\''b' ESCAPE '\')");
}

TEST_CASE("patternPredicate: single quotes are doubled") {
  CHECK_STR(patternPredicate("table_name", S("o'reilly"), false), R"(table_name LIKE 'o''reilly' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("''"), false), R"(table_name LIKE '''''' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("'%"), false), R"(table_name LIKE '''%' ESCAPE '\')");
}

TEST_CASE("patternPredicate: literal mode compares with = and treats % and _ as ordinary characters") {
  CHECK_STR(patternPredicate("table_name", S("orders"), true), "table_name = 'orders'");
  CHECK_STR(patternPredicate("table_name", S("%"), true), "table_name = '%'");
  CHECK_STR(patternPredicate("table_name", S("a%b_c"), true), "table_name = 'a%b_c'");
  CHECK_STR(patternPredicate("table_name", S("O'Neil"), true), "table_name = 'O''Neil'");
  // Trino string literals give backslashes no meaning, so nothing is doubled in literal mode.
  CHECK_STR(patternPredicate("table_name", S(R"(C:\temp)"), true), R"(table_name = 'C:\temp')");
  CHECK_STR(patternPredicate("table_name", S(R"(my\_table)"), true), R"(table_name = 'my\_table')");
}

TEST_CASE("patternPredicate: literal mode applies the SQL_ATTR_METADATA_ID identifier rules") {
  CHECK_STR(patternPredicate("table_name", S("\"My Table\""), true), "table_name = 'My Table'");
  CHECK_STR(patternPredicate("table_name", S("\"na\"\"me\""), true), "table_name = 'na\"me'");
  CHECK_STR(patternPredicate("table_name", S("\"%\""), true), "table_name = '%'");
  CHECK_STR(patternPredicate("table_name", S("orders   "), true), "table_name = 'orders'");
  CHECK_STR(patternPredicate("table_name", S("   orders"), true), "table_name = '   orders'");
  // Trino stores identifiers in lower case exactly as written, so no case folding ever happens.
  CHECK_STR(patternPredicate("table_name", S("Orders"), true), "table_name = 'Orders'");
  CHECK_STR(patternPredicate("table_name", S("\"Orders\""), true), "table_name = 'Orders'");
}

TEST_CASE("patternPredicate: pattern mode never applies identifier rules") {
  CHECK_STR(patternPredicate("table_name", S("\"orders\""), false), R"(table_name LIKE '"orders"' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("orders  "), false), R"(table_name LIKE 'orders  ' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("Orders"), false), R"(table_name LIKE 'Orders' ESCAPE '\')");
}

TEST_CASE("patternPredicate: the column expression is used verbatim") {
  CHECK_STR(patternPredicate("lower(table_name)", S("x"), false), R"(lower(table_name) LIKE 'x' ESCAPE '\')");
  CHECK_STR(patternPredicate("t.table_cat", S("x"), true), "t.table_cat = 'x'");
}

TEST_CASE("patternPredicate: UTF-8 text passes through untouched") {
  CHECK_STR(patternPredicate("table_name", S("müşteri_%"), false), R"(table_name LIKE 'müşteri_%' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("日本語"), false), R"(table_name LIKE '日本語' ESCAPE '\')");
  CHECK_STR(patternPredicate("table_name", S("\"Müşteri\""), true), "table_name = 'Müşteri'");
}

// ==================================================================================================
// normalizeIdentifierArgument
// ==================================================================================================

TEST_CASE("normalizeIdentifierArgument: plain identifiers lose trailing blanks only") {
  CHECK_STR(normalizeIdentifierArgument("orders"), "orders");
  CHECK_STR(normalizeIdentifierArgument("orders   "), "orders");
  CHECK_STR(normalizeIdentifierArgument("   orders"), "   orders");
  CHECK_STR(normalizeIdentifierArgument("  orders  "), "  orders");
  CHECK_STR(normalizeIdentifierArgument("my table  "), "my table");
  // Only the blank character counts as a trailing blank.
  CHECK_STR(normalizeIdentifierArgument("orders\t"), "orders\t");
  CHECK_STR(normalizeIdentifierArgument(""), "");
  CHECK_STR(normalizeIdentifierArgument("     "), "");
}

TEST_CASE("normalizeIdentifierArgument: delimited identifiers lose the quotes and doubled quotes") {
  CHECK_STR(normalizeIdentifierArgument("\"My Table\""), "My Table");
  CHECK_STR(normalizeIdentifierArgument("\"na\"\"me\""), "na\"me");
  CHECK_STR(normalizeIdentifierArgument("\"\"\"\""), "\"");  // "" "" -> a single quote character
  CHECK_STR(normalizeIdentifierArgument("\"\""), "");
  CHECK_STR(normalizeIdentifierArgument("\"abc  \""), "abc  ");  // blanks inside delimiters are significant
  CHECK_STR(normalizeIdentifierArgument("\"  abc\""), "  abc");
  CHECK_STR(normalizeIdentifierArgument("\"%_\""), "%_");
}

TEST_CASE("normalizeIdentifierArgument: never folds case and ignores a lone quote") {
  CHECK_STR(normalizeIdentifierArgument("Orders"), "Orders");
  CHECK_STR(normalizeIdentifierArgument("\"Orders\""), "Orders");
  CHECK_STR(normalizeIdentifierArgument("\""), "\"");
  CHECK_STR(normalizeIdentifierArgument("\"open"), "\"open");
  CHECK_STR(normalizeIdentifierArgument("close\""), "close\"");
}

// ==================================================================================================
// ordinaryPredicate
// ==================================================================================================

TEST_CASE("ordinaryPredicate: null pointer is no filter, everything else is an equality") {
  CHECK_STR(ordinaryPredicate("table_cat", std::nullopt, false), "");
  CHECK_STR(ordinaryPredicate("table_cat", std::nullopt, true), "");
  CHECK_STR(ordinaryPredicate("table_cat", S(""), false), "table_cat = ''");
  CHECK_STR(ordinaryPredicate("table_cat", S("tpch"), false), "table_cat = 'tpch'");
  CHECK_STR(ordinaryPredicate("procedure_cat", S("tpch"), false), "procedure_cat = 'tpch'");
}

TEST_CASE("ordinaryPredicate: wildcards and backslashes have no meaning in an ordinary argument") {
  CHECK_STR(ordinaryPredicate("table_cat", S("%"), false), "table_cat = '%'");
  CHECK_STR(ordinaryPredicate("table_cat", S("a_b%"), false), "table_cat = 'a_b%'");
  CHECK_STR(ordinaryPredicate("table_cat", S(R"(a\b)"), false), R"(table_cat = 'a\b')");
  CHECK_STR(ordinaryPredicate("table_cat", S("it's"), false), "table_cat = 'it''s'");
}

TEST_CASE("ordinaryPredicate: identifier rules apply only in literal mode") {
  CHECK_STR(ordinaryPredicate("table_cat", S("\"tpch\""), false), "table_cat = '\"tpch\"'");
  CHECK_STR(ordinaryPredicate("table_cat", S("tpch  "), false), "table_cat = 'tpch  '");
  CHECK_STR(ordinaryPredicate("table_cat", S("\"tpch\""), true), "table_cat = 'tpch'");
  CHECK_STR(ordinaryPredicate("table_cat", S("tpch  "), true), "table_cat = 'tpch'");
  CHECK_STR(ordinaryPredicate("table_cat", S("\"Tp\"\"ch\""), true), "table_cat = 'Tp\"ch'");
}

// ==================================================================================================
// tableTypesPredicate / whereClause
// ==================================================================================================

TEST_CASE("tableTypesPredicate: IN list in the given order, empty for no types") {
  CHECK_STR(tableTypesPredicate({}), "");
  CHECK_STR(tableTypesPredicate({"TABLE"}), "table_type IN ('TABLE')");
  CHECK_STR(tableTypesPredicate({"TABLE", "VIEW"}), "table_type IN ('TABLE', 'VIEW')");
  CHECK_STR(tableTypesPredicate({"VIEW", "TABLE"}), "table_type IN ('VIEW', 'TABLE')");
  CHECK_STR(tableTypesPredicate({"SYSTEM TABLE"}), "table_type IN ('SYSTEM TABLE')");
  CHECK_STR(tableTypesPredicate({"IT'S"}), "table_type IN ('IT''S')");
}

TEST_CASE("whereClause: joins the non-empty fragments with AND") {
  CHECK_STR(whereClause({}), "");
  CHECK_STR(whereClause({""}), "");
  CHECK_STR(whereClause({"", ""}), "");
  CHECK_STR(whereClause({"a = 1"}), " WHERE a = 1");
  CHECK_STR(whereClause({"a = 1", "b = 2"}), " WHERE a = 1 AND b = 2");
  CHECK_STR(whereClause({"", "a = 1", "", "c = 3", ""}), " WHERE a = 1 AND c = 3");
}

// ==================================================================================================
// parseTableTypes
// ==================================================================================================

TEST_CASE("parseTableTypes: null, empty, blank and '%' mean no filter") {
  CHECK_STR(types(std::nullopt), "");
  CHECK_STR(types(S("")), "");
  CHECK_STR(types(S("   ")), "");
  CHECK_STR(types(S("%")), "");
  CHECK_STR(types(S("  %  ")), "");
  CHECK_STR(types(S("'%'")), "");
}

TEST_CASE("parseTableTypes: plain and quoted comma lists") {
  CHECK_STR(types(S("TABLE")), "TABLE");
  CHECK_STR(types(S("TABLE,VIEW")), "TABLE|VIEW");
  CHECK_STR(types(S("'TABLE','VIEW'")), "TABLE|VIEW");
  CHECK_STR(types(S("  'TABLE' ,  'VIEW'  ")), "TABLE|VIEW");
  CHECK_STR(types(S("'TABLE',VIEW")), "TABLE|VIEW");
  CHECK_STR(types(S("TABLE, VIEW, SYSTEM TABLE")), "TABLE|VIEW|SYSTEM TABLE");
  CHECK_STR(types(S("GLOBAL TEMPORARY,LOCAL TEMPORARY,ALIAS,SYNONYM")), "GLOBAL TEMPORARY|LOCAL TEMPORARY|ALIAS|SYNONYM");
}

TEST_CASE("parseTableTypes: values are upper-cased and internal whitespace is collapsed") {
  CHECK_STR(types(S("table, View")), "TABLE|VIEW");
  CHECK_STR(types(S("'view'")), "VIEW");
  CHECK_STR(types(S("SYSTEM TABLE")), "SYSTEM TABLE");
  CHECK_STR(types(S("system   table")), "SYSTEM TABLE");
  CHECK_STR(types(S("SYSTEM\tTABLE")), "SYSTEM TABLE");
  CHECK_STR(types(S("'SYSTEM TABLE'")), "SYSTEM TABLE");
  CHECK_STR(types(S("' system  table '")), "SYSTEM TABLE");
}

TEST_CASE("parseTableTypes: duplicates are dropped and first-seen order is kept") {
  CHECK_STR(types(S("TABLE,TABLE,table,'TABLE'")), "TABLE");
  CHECK_STR(types(S("VIEW,TABLE")), "VIEW|TABLE");
  CHECK_STR(types(S("VIEW,TABLE,VIEW")), "VIEW|TABLE");
}

TEST_CASE("parseTableTypes: '%' anywhere in the list removes the filter entirely") {
  CHECK_STR(types(S("TABLE,%,VIEW")), "");
  CHECK_STR(types(S("TABLE,'%'")), "");
  CHECK_STR(types(S("%,TABLE")), "");
}

TEST_CASE("parseTableTypes: empty items are skipped") {
  CHECK_STR(types(S("TABLE,,VIEW")), "TABLE|VIEW");
  CHECK_STR(types(S("TABLE,")), "TABLE");
  CHECK_STR(types(S(",VIEW")), "VIEW");
  CHECK_STR(types(S(",")), "");
  CHECK_STR(types(S("''")), "");
  CHECK_STR(types(S("'',TABLE")), "TABLE");
  CHECK_STR(types(S("' '")), "");
}

TEST_CASE("parseTableTypes: doubled quotes inside a quoted item are undone") {
  CHECK_STR(types(S("'IT''S'")), "IT'S");
  CHECK_STR(types(S("'A''B',C")), "A'B|C");
}

// ==================================================================================================
// detectSpecialEnumeration
// ==================================================================================================

TEST_CASE("detectSpecialEnumeration: ordinary argument combinations") {
  CHECK_STR(detect(std::nullopt, std::nullopt, std::nullopt), "None");
  CHECK_STR(detect(S("tpch"), S("tiny"), S("%")), "None");
  CHECK_STR(detect(S("tpch"), std::nullopt, std::nullopt), "None");
  CHECK_STR(detect(S("%"), S("%"), S("%")), "None");
  CHECK_STR(detect(S(""), S(""), S("")), "None");
  CHECK_STR(detect(std::nullopt, std::nullopt, std::nullopt, S("TABLE,VIEW")), "None");
}

TEST_CASE("detectSpecialEnumeration: SQL_ALL_CATALOGS needs '%' catalog and empty schema and table") {
  CHECK_STR(detect(S("%"), S(""), S("")), "AllCatalogs");
  CHECK_STR(detect(S("%"), S(""), S(""), S("")), "AllCatalogs");
  CHECK_STR(detect(S("%"), S(""), S(""), S("TABLE")), "AllCatalogs");  // TableType plays no part
  // A null pointer is not an empty string, and a longer pattern is not SQL_ALL_CATALOGS.
  CHECK_STR(detect(S("%"), std::nullopt, S("")), "None");
  CHECK_STR(detect(S("%"), S(""), std::nullopt), "None");
  CHECK_STR(detect(S("%"), S(""), S("%")), "None");
  CHECK_STR(detect(S("%%"), S(""), S("")), "None");
  CHECK_STR(detect(S(" %"), S(""), S("")), "None");
}

TEST_CASE("detectSpecialEnumeration: SQL_ALL_SCHEMAS needs empty catalog, '%' schema, empty table") {
  CHECK_STR(detect(S(""), S("%"), S("")), "AllSchemas");
  CHECK_STR(detect(S(""), S("%"), S(""), S("VIEW")), "AllSchemas");
  CHECK_STR(detect(S(""), S("%"), std::nullopt), "None");
  CHECK_STR(detect(S(""), S("%"), S("%")), "None");
  CHECK_STR(detect(S(""), S("%%"), S("")), "None");
}

TEST_CASE("detectSpecialEnumeration: '%' schema with a null or named catalog lists that catalog's schemas") {
  CHECK_STR(detect(std::nullopt, S("%"), S("")), "CatalogSchemas");
  CHECK_STR(detect(S("tpch"), S("%"), S("")), "CatalogSchemas");
  CHECK_STR(detect(S("tp%"), S("%"), S("")), "CatalogSchemas");
  CHECK_STR(detect(S("%"), S("%"), S("")), "CatalogSchemas");  // every catalog, catalog column filled
  CHECK_STR(detect(S("tpch"), S("%"), std::nullopt), "None");
  CHECK_STR(detect(S("tpch"), S("%"), S("%")), "None");
}

TEST_CASE("detectSpecialEnumeration: SQL_ALL_TABLE_TYPES needs '%' type and three empty names") {
  CHECK_STR(detect(S(""), S(""), S(""), S("%")), "AllTableTypes");
  CHECK_STR(detect(std::nullopt, S(""), S(""), S("%")), "None");
  CHECK_STR(detect(S(""), std::nullopt, S(""), S("%")), "None");
  CHECK_STR(detect(S(""), S(""), std::nullopt, S("%")), "None");
  CHECK_STR(detect(S(""), S(""), S(""), S("TABLE,%")), "None");  // only the bare "%" is the marker
  CHECK_STR(detect(S(""), S(""), S(""), S("")), "None");
  CHECK_STR(detect(S(""), S(""), S(""), std::nullopt), "None");
}

TEST_CASE("detectSpecialEnumeration: precedence when several markers are present") {
  CHECK_STR(detect(S("%"), S(""), S(""), S("%")), "AllCatalogs");
  CHECK_STR(detect(S(""), S("%"), S(""), S("%")), "AllSchemas");
  CHECK_STR(detect(S("%"), S("%"), S(""), S("%")), "CatalogSchemas");
}

TEST_CASE("detectSpecialEnumeration: SQL_ATTR_METADATA_ID does not disable the enumerations") {
  // Power BI and the Driver Manager send SQL_ALL_CATALOGS regardless of the attribute.
  CHECK_STR(detect(S("%"), S(""), S(""), std::nullopt, true), "AllCatalogs");
  CHECK_STR(detect(S(""), S("%"), S(""), std::nullopt, true), "AllSchemas");
  CHECK_STR(detect(S(""), S(""), S(""), S("%"), true), "AllTableTypes");
  CHECK_STR(detect(S("tpch"), S("%"), S(""), std::nullopt, true), "CatalogSchemas");
}

// ==================================================================================================
// Query builders
// ==================================================================================================

TEST_CASE("buildCatalogsQuery / buildTableTypesQuery: fixed text") {
  CHECK_STR(buildCatalogsQuery(), "SELECT table_cat FROM system.jdbc.catalogs ORDER BY table_cat");
  CHECK_STR(buildTableTypesQuery(), "SELECT table_type FROM system.jdbc.table_types ORDER BY table_type");
}

TEST_CASE("buildSchemasQuery: no arguments, one argument, both arguments") {
  CHECK_STR(buildSchemasQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt)),
            query(kSchemasSelect, "", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S("tpch"), std::nullopt, std::nullopt)),
            query(kSchemasSelect, R"( WHERE table_catalog LIKE 'tpch' ESCAPE '\')", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(std::nullopt, S("ti%"), std::nullopt)),
            query(kSchemasSelect, R"( WHERE table_schem LIKE 'ti%' ESCAPE '\')", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S("tpch"), S("tiny"), std::nullopt)),
            query(kSchemasSelect, R"( WHERE table_catalog LIKE 'tpch' ESCAPE '\' AND table_schem LIKE 'tiny' ESCAPE '\')",
                  kSchemasOrder));
}

TEST_CASE("buildSchemasQuery: '%' is dropped, empty string matches nothing, literal mode uses =") {
  CHECK_STR(buildSchemasQuery(makeArgs(S("%"), S("%"), std::nullopt)), query(kSchemasSelect, "", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S(""), std::nullopt, std::nullopt)),
            query(kSchemasSelect, " WHERE table_catalog = ''", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S("tpch"), S("Tiny"), std::nullopt, std::nullopt, std::nullopt, true)),
            query(kSchemasSelect, " WHERE table_catalog = 'tpch' AND table_schem = 'Tiny'", kSchemasOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S("\"tpch\""), S("ti%"), std::nullopt, std::nullopt, std::nullopt, true)),
            query(kSchemasSelect, " WHERE table_catalog = 'tpch' AND table_schem = 'ti%'", kSchemasOrder));
  // Table and column arguments never leak into the schema query.
  CHECK_STR(buildSchemasQuery(makeArgs(std::nullopt, std::nullopt, S("orders"), S("id"), S("TABLE"))),
            query(kSchemasSelect, "", kSchemasOrder));
}

TEST_CASE("buildTablesQuery: no arguments yields the JDBC column list and ORDER BY") {
  CHECK_STR(buildTablesQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt)),
            "SELECT table_cat, table_schem, table_name, table_type, remarks, type_cat, type_schem, type_name, "
            "self_referencing_col_name, ref_generation FROM system.jdbc.tables "
            "ORDER BY table_type, table_cat, table_schem, table_name");
}

TEST_CASE("buildTablesQuery: every argument is a search pattern, in catalog/schema/table/type order") {
  CHECK_STR(buildTablesQuery(makeArgs(S("tpch"), S("tiny"), S("ord%"), std::nullopt, S("TABLE,VIEW"))),
            query(kTablesSelect,
                  R"( WHERE table_cat LIKE 'tpch' ESCAPE '\' AND table_schem LIKE 'tiny' ESCAPE '\')"
                  R"( AND table_name LIKE 'ord%' ESCAPE '\' AND table_type IN ('TABLE', 'VIEW'))",
                  kTablesOrder));
  CHECK_STR(buildTablesQuery(makeArgs(std::nullopt, std::nullopt, S("my\\_t_"), std::nullopt, std::nullopt)),
            query(kTablesSelect, R"( WHERE table_name LIKE 'my\_t_' ESCAPE '\')", kTablesOrder));
  CHECK_STR(buildTablesQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt, std::nullopt, S("'VIEW'"))),
            query(kTablesSelect, " WHERE table_type IN ('VIEW')", kTablesOrder));
}

TEST_CASE("buildTablesQuery: '%' arguments and '%' table types add no predicate") {
  CHECK_STR(buildTablesQuery(makeArgs(S("%"), S("%"), S("%"), std::nullopt, S("%"))),
            query(kTablesSelect, "", kTablesOrder));
  CHECK_STR(buildTablesQuery(makeArgs(S("tpch"), S("%"), S("%"), std::nullopt, S("TABLE,%"))),
            query(kTablesSelect, R"( WHERE table_cat LIKE 'tpch' ESCAPE '\')", kTablesOrder));
  // The column argument does not belong to SQLTables and is ignored.
  CHECK_STR(buildTablesQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt, S("id"), std::nullopt)),
            query(kTablesSelect, "", kTablesOrder));
}

TEST_CASE("buildTablesQuery: literal mode compares every name with =") {
  CHECK_STR(buildTablesQuery(makeArgs(S("tpch"), S("tiny"), S("ord%"), std::nullopt, S("TABLE"), true)),
            query(kTablesSelect,
                  " WHERE table_cat = 'tpch' AND table_schem = 'tiny' AND table_name = 'ord%'"
                  " AND table_type IN ('TABLE')",
                  kTablesOrder));
  CHECK_STR(buildTablesQuery(makeArgs(S("\"tpch\""), S("tiny  "), S("\"Or\"\"ders\""), std::nullopt, std::nullopt, true)),
            query(kTablesSelect, " WHERE table_cat = 'tpch' AND table_schem = 'tiny' AND table_name = 'Or\"ders'",
                  kTablesOrder));
  // "%" is a literal percent sign in literal mode, even for the catalog.
  CHECK_STR(buildTablesQuery(makeArgs(S("%"), std::nullopt, std::nullopt, std::nullopt, std::nullopt, true)),
            query(kTablesSelect, " WHERE table_cat = '%'", kTablesOrder));
}

TEST_CASE("buildTablesQuery: the call sequences Power BI Desktop issues") {
  // SQLTablesW(catalog, NULL, NULL, NULL)
  CHECK_STR(buildTablesQuery(makeArgs(S("tpch"), std::nullopt, std::nullopt)),
            query(kTablesSelect, R"( WHERE table_cat LIKE 'tpch' ESCAPE '\')", kTablesOrder));
  // SQLTablesW(catalog, schema, "%", "TABLE,VIEW")
  CHECK_STR(buildTablesQuery(makeArgs(S("tpch"), S("tiny"), S("%"), std::nullopt, S("TABLE,VIEW"))),
            query(kTablesSelect,
                  R"( WHERE table_cat LIKE 'tpch' ESCAPE '\' AND table_schem LIKE 'tiny' ESCAPE '\')"
                  " AND table_type IN ('TABLE', 'VIEW')",
                  kTablesOrder));
}

TEST_CASE("buildColumnsQuery: no arguments yields the JDBC column list and ORDER BY") {
  CHECK_STR(buildColumnsQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt)),
            "SELECT table_cat, table_schem, table_name, column_name, data_type, type_name, column_size, "
            "buffer_length, decimal_digits, num_prec_radix, nullable, remarks, column_def, sql_data_type, "
            "sql_datetime_sub, char_octet_length, ordinal_position, is_nullable FROM system.jdbc.columns "
            "ORDER BY table_cat, table_schem, table_name, ordinal_position");
}

TEST_CASE("buildColumnsQuery: catalog is an ordinary argument, the other three are patterns") {
  CHECK_STR(buildColumnsQuery(makeArgs(S("tpch"), S("tiny"), S("orders"), S("%"))),
            query(kColumnsSelect,
                  R"( WHERE table_cat = 'tpch' AND table_schem LIKE 'tiny' ESCAPE '\' AND table_name LIKE 'orders' ESCAPE '\')",
                  kColumnsOrder));
  CHECK_STR(buildColumnsQuery(makeArgs(S("tpch"), S("tiny"), S("orders"), S("o\\_%"))),
            query(kColumnsSelect,
                  R"( WHERE table_cat = 'tpch' AND table_schem LIKE 'tiny' ESCAPE '\' AND table_name LIKE 'orders' ESCAPE '\')"
                  R"( AND column_name LIKE 'o\_%' ESCAPE '\')",
                  kColumnsOrder));
  // "%" as the catalog is a literal percent sign: SQLColumns' CatalogName never accepts patterns.
  CHECK_STR(buildColumnsQuery(makeArgs(S("%"), std::nullopt, std::nullopt, std::nullopt)),
            query(kColumnsSelect, " WHERE table_cat = '%'", kColumnsOrder));
  CHECK_STR(buildColumnsQuery(makeArgs(std::nullopt, std::nullopt, S("cust%"), std::nullopt)),
            query(kColumnsSelect, R"( WHERE table_name LIKE 'cust%' ESCAPE '\')", kColumnsOrder));
  // The SQLTables-only TableType argument never reaches SQLColumns.
  CHECK_STR(buildColumnsQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt, std::nullopt, S("TABLE"))),
            query(kColumnsSelect, "", kColumnsOrder));
}

TEST_CASE("buildColumnsQuery: literal mode") {
  CHECK_STR(buildColumnsQuery(makeArgs(S("\"tpch\""), S("tiny"), S("Orders "), S("\"Order Key\""), std::nullopt, true)),
            query(kColumnsSelect,
                  " WHERE table_cat = 'tpch' AND table_schem = 'tiny' AND table_name = 'Orders' AND column_name = 'Order Key'",
                  kColumnsOrder));
  CHECK_STR(buildColumnsQuery(makeArgs(std::nullopt, std::nullopt, S("%"), S("_"), std::nullopt, true)),
            query(kColumnsSelect, " WHERE table_name = '%' AND column_name = '_'", kColumnsOrder));
}

TEST_CASE("buildProceduresQuery: fixed text, catalog ordinary, schema and name patterns") {
  CHECK_STR(buildProceduresQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt)),
            "SELECT procedure_cat, procedure_schem, procedure_name, remarks, procedure_type, specific_name "
            "FROM system.jdbc.procedures ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name");
  // CatalogArgs::table carries the ProcName argument.
  CHECK_STR(buildProceduresQuery(makeArgs(S("system"), S("run%"), S("proc\\_1"))),
            query(kProceduresSelect,
                  R"( WHERE procedure_cat = 'system' AND procedure_schem LIKE 'run%' ESCAPE '\')"
                  R"( AND procedure_name LIKE 'proc\_1' ESCAPE '\')",
                  kProceduresOrder));
  CHECK_STR(buildProceduresQuery(makeArgs(S("%"), S("%"), S("%"))),
            query(kProceduresSelect, " WHERE procedure_cat = '%'", kProceduresOrder));
  CHECK_STR(buildProceduresQuery(makeArgs(S("system"), S("runtime"), S("\"Kill\""), S("ignored"), std::nullopt, true)),
            query(kProceduresSelect, " WHERE procedure_cat = 'system' AND procedure_schem = 'runtime' AND procedure_name = 'Kill'",
                  kProceduresOrder));
}

TEST_CASE("buildProcedureColumnsQuery: fixed text and all four arguments") {
  CHECK_STR(buildProcedureColumnsQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt)),
            "SELECT procedure_cat, procedure_schem, procedure_name, column_name, column_type, data_type, type_name, "
            "precision, length, scale, radix, nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
            "char_octet_length, ordinal_position, is_nullable, specific_name FROM system.jdbc.procedure_columns "
            "ORDER BY procedure_cat, procedure_schem, procedure_name, specific_name, ordinal_position");
  CHECK_STR(buildProcedureColumnsQuery(makeArgs(S("system"), S("runtime"), S("kill_query"), S("q%"))),
            query(kProcColumnsSelect,
                  R"( WHERE procedure_cat = 'system' AND procedure_schem LIKE 'runtime' ESCAPE '\')"
                  R"( AND procedure_name LIKE 'kill_query' ESCAPE '\' AND column_name LIKE 'q%' ESCAPE '\')",
                  kProcColumnsOrder));
  CHECK_STR(buildProcedureColumnsQuery(makeArgs(std::nullopt, std::nullopt, std::nullopt, S("query_id"))),
            query(kProcColumnsSelect, R"( WHERE column_name LIKE 'query_id' ESCAPE '\')", kProcColumnsOrder));
  CHECK_STR(buildProcedureColumnsQuery(makeArgs(S("system"), S("%"), S("%"), S("\"Q\""), std::nullopt, true)),
            query(kProcColumnsSelect, " WHERE procedure_cat = 'system' AND procedure_schem = '%' AND procedure_name = '%' AND column_name = 'Q'",
                  kProcColumnsOrder));
}

TEST_CASE("query builders: hostile argument text cannot break out of the literal") {
  const std::string hostile = "x'; DROP TABLE t; --";
  const std::string tables = buildTablesQuery(makeArgs(S(hostile.c_str()), S(hostile.c_str()), S(hostile.c_str()),
                                                       std::nullopt, S(hostile.c_str())));
  // The quote after "x" must always be doubled: an undoubled "x'; DROP" would terminate the literal.
  CHECK(tables.find("x'; DROP") == std::string::npos);
  CHECK(tables.find("'x''; DROP TABLE t; --'") != std::string::npos);
  CHECK(tables.find("table_type IN ('X''; DROP TABLE T; --')") != std::string::npos);
  CHECK_STR(buildColumnsQuery(makeArgs(S(hostile.c_str()), std::nullopt, std::nullopt, std::nullopt)),
            query(kColumnsSelect, " WHERE table_cat = 'x''; DROP TABLE t; --'", kColumnsOrder));
  CHECK_STR(buildSchemasQuery(makeArgs(S(hostile.c_str()), std::nullopt, std::nullopt, std::nullopt, std::nullopt, true)),
            query(kSchemasSelect, " WHERE table_catalog = 'x''; DROP TABLE t; --'", kSchemasOrder));
  // Newlines and NUL-free control characters are just characters inside the literal.
  CHECK_STR(buildTablesQuery(makeArgs(std::nullopt, std::nullopt, S("a\nb"))),
            query(kTablesSelect, " WHERE table_name LIKE 'a\nb' ESCAPE '\\'", kTablesOrder));
}

// ==================================================================================================
// Result layouts of the functions that need no server
// ==================================================================================================

TEST_CASE("catalogPrimaryKeys: spec layout, empty") {
  Connection conn;
  auto src = catalogPrimaryKeys(conn, makeArgs(S("tpch"), S("tiny"), S("orders")));
  REQUIRE(src != nullptr);
  checkColumnNames(src->columns(), {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"});
  const auto& cols = src->columns();
  CHECK(cols[0].type.category == TypeCategory::Varchar);
  CHECK(cols[4].type.category == TypeCategory::SmallInt);
  CHECK_EQ(cols[4].odbc.conciseType, SQL_SMALLINT);
  CHECK_EQ(cols[0].odbc.nullable, SQL_NULLABLE);
  CHECK_EQ(cols[2].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[3].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[4].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[5].odbc.nullable, SQL_NULLABLE);
  checkEmpty(*src);
}

TEST_CASE("catalogForeignKeys: spec layout, empty") {
  Connection conn;
  auto src = catalogForeignKeys(conn, makeArgs(S("tpch"), S("tiny"), S("orders")),
                                makeArgs(S("tpch"), S("tiny"), S("lineitem")));
  REQUIRE(src != nullptr);
  checkColumnNames(src->columns(), {"PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME", "FKTABLE_CAT",
                                    "FKTABLE_SCHEM", "FKTABLE_NAME", "FKCOLUMN_NAME", "KEY_SEQ", "UPDATE_RULE",
                                    "DELETE_RULE", "FK_NAME", "PK_NAME", "DEFERRABILITY"});
  const auto& cols = src->columns();
  CHECK_EQ(cols[8].odbc.conciseType, SQL_SMALLINT);
  CHECK_EQ(cols[8].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[9].odbc.nullable, SQL_NULLABLE);
  CHECK_EQ(cols[13].odbc.conciseType, SQL_SMALLINT);
  checkEmpty(*src);
  // Null-pointer arguments on both sides are fine too.
  auto none = catalogForeignKeys(conn, makeArgs(std::nullopt, std::nullopt, std::nullopt),
                                 makeArgs(std::nullopt, std::nullopt, std::nullopt));
  REQUIRE(none != nullptr);
  CHECK_EQ(none->columns().size(), size_t{14});
  checkEmpty(*none);
}

TEST_CASE("catalogStatistics: spec layout, empty, arguments validated") {
  Connection conn;
  const CatalogArgs args = makeArgs(S("tpch"), S("tiny"), S("orders"));
  auto src = catalogStatistics(conn, args, SQL_INDEX_ALL, SQL_QUICK);
  REQUIRE(src != nullptr);
  checkColumnNames(src->columns(), {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "NON_UNIQUE", "INDEX_QUALIFIER",
                                    "INDEX_NAME", "TYPE", "ORDINAL_POSITION", "COLUMN_NAME", "ASC_OR_DESC",
                                    "CARDINALITY", "PAGES", "FILTER_CONDITION"});
  const auto& cols = src->columns();
  CHECK_EQ(cols[6].odbc.conciseType, SQL_SMALLINT);
  CHECK_EQ(cols[6].odbc.nullable, SQL_NO_NULLS);
  CHECK(cols[9].type.category == TypeCategory::Varchar);
  CHECK_EQ(cols[9].odbc.columnSize, static_cast<SQLULEN>(1));
  CHECK(cols[10].type.category == TypeCategory::Integer);
  CHECK_EQ(cols[10].odbc.conciseType, SQL_INTEGER);
  checkEmpty(*src);

  // Every valid combination answers; every invalid code has its own SQLSTATE.
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, SQL_INDEX_UNIQUE, SQL_ENSURE); }), "<none>");
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, SQL_INDEX_UNIQUE, SQL_QUICK); }), "<none>");
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, SQL_INDEX_ALL, SQL_ENSURE); }), "<none>");
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, 2, SQL_QUICK); }), sqlstate::kUniquenessOptionOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, 0xFFFF, SQL_QUICK); }),
            sqlstate::kUniquenessOptionOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, SQL_INDEX_ALL, 2); }), sqlstate::kAccuracyOptionOutOfRange);
  // The Unique argument is checked first.
  CHECK_STR(thrownSqlstate([&] { catalogStatistics(conn, args, 7, 7); }), sqlstate::kUniquenessOptionOutOfRange);
}

TEST_CASE("catalogSpecialColumns: spec layout, empty, arguments validated") {
  Connection conn;
  const CatalogArgs args = makeArgs(S("tpch"), S("tiny"), S("orders"));
  auto src = catalogSpecialColumns(conn, SQL_BEST_ROWID, args, SQL_SCOPE_CURROW, SQL_NULLABLE);
  REQUIRE(src != nullptr);
  checkColumnNames(src->columns(), {"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH",
                                    "DECIMAL_DIGITS", "PSEUDO_COLUMN"});
  const auto& cols = src->columns();
  CHECK_EQ(cols[0].odbc.conciseType, SQL_SMALLINT);
  CHECK_EQ(cols[1].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[2].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[3].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(cols[4].odbc.conciseType, SQL_INTEGER);
  checkEmpty(*src);

  for (SQLUSMALLINT idType : {SQL_BEST_ROWID, SQL_ROWVER}) {
    for (SQLUSMALLINT scope : {SQL_SCOPE_CURROW, SQL_SCOPE_TRANSACTION, SQL_SCOPE_SESSION}) {
      for (SQLUSMALLINT nullable : {SQL_NO_NULLS, SQL_NULLABLE}) {
        CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, idType, args, scope, nullable); }), "<none>");
      }
    }
  }
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, 0, args, SQL_SCOPE_CURROW, SQL_NULLABLE); }),
            sqlstate::kColumnTypeOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, 3, args, SQL_SCOPE_CURROW, SQL_NULLABLE); }),
            sqlstate::kColumnTypeOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, SQL_BEST_ROWID, args, 3, SQL_NULLABLE); }),
            sqlstate::kScopeTypeOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, SQL_BEST_ROWID, args, SQL_SCOPE_CURROW, 2); }),
            sqlstate::kNullableTypeOutOfRange);
  // Validation order: identifier type, then scope, then nullable.
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, 9, args, 9, 9); }), sqlstate::kColumnTypeOutOfRange);
  CHECK_STR(thrownSqlstate([&] { catalogSpecialColumns(conn, SQL_ROWVER, args, 9, 9); }), sqlstate::kScopeTypeOutOfRange);
}

TEST_CASE("catalogTablePrivileges / catalogColumnPrivileges: spec layouts, empty") {
  Connection conn;
  auto tables = catalogTablePrivileges(conn, makeArgs(S("tpch"), S("tiny"), S("%")));
  REQUIRE(tables != nullptr);
  checkColumnNames(tables->columns(),
                   {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "GRANTOR", "GRANTEE", "PRIVILEGE", "IS_GRANTABLE"});
  CHECK_EQ(tables->columns()[2].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(tables->columns()[4].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(tables->columns()[5].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(tables->columns()[6].odbc.nullable, SQL_NULLABLE);
  CHECK_EQ(tables->columns()[6].odbc.columnSize, static_cast<SQLULEN>(3));  // "YES" / "NO"
  checkEmpty(*tables);

  auto columns = catalogColumnPrivileges(conn, makeArgs(S("tpch"), S("tiny"), S("orders"), S("%")));
  REQUIRE(columns != nullptr);
  checkColumnNames(columns->columns(), {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "GRANTOR", "GRANTEE",
                                        "PRIVILEGE", "IS_GRANTABLE"});
  CHECK_EQ(columns->columns()[3].odbc.nullable, SQL_NO_NULLS);
  CHECK_EQ(columns->columns()[4].odbc.nullable, SQL_NULLABLE);
  checkEmpty(*columns);
}

TEST_CASE("empty catalog results: identifier columns follow the connection's character-type setting") {
  Connection wide;
  auto w = catalogPrimaryKeys(wide, makeArgs(std::nullopt, std::nullopt, std::nullopt));
  REQUIRE(w != nullptr);
  CHECK_EQ(w->columns()[0].odbc.conciseType, SQL_WVARCHAR);
  CHECK_EQ(w->columns()[3].odbc.conciseType, SQL_WVARCHAR);
  CHECK_EQ(w->columns()[0].odbc.columnSize, static_cast<SQLULEN>(256));

  Connection narrow;
  narrow.config.unicodeTypes = false;
  auto n = catalogPrimaryKeys(narrow, makeArgs(std::nullopt, std::nullopt, std::nullopt));
  REQUIRE(n != nullptr);
  CHECK_EQ(n->columns()[0].odbc.conciseType, SQL_VARCHAR);
  CHECK_EQ(n->columns()[3].odbc.conciseType, SQL_VARCHAR);
  // Numeric columns are unaffected.
  CHECK_EQ(n->columns()[4].odbc.conciseType, SQL_SMALLINT);
}

TEST_CASE("empty catalog results: bounded lengths keep identifiers (W)VARCHAR under UnboundedVarcharAsLongVarchar") {
  Connection conn;
  conn.config.unboundedVarcharAsLong = true;
  auto src = catalogStatistics(conn, makeArgs(std::nullopt, std::nullopt, S("t")), SQL_INDEX_ALL, SQL_QUICK);
  REQUIRE(src != nullptr);
  for (const ResultColumn& c : src->columns()) {
    CHECK(c.odbc.conciseType != SQL_WLONGVARCHAR);
    CHECK(c.odbc.conciseType != SQL_LONGVARCHAR);
  }
}

// ==================================================================================================
// Functions that must query the server refuse on an unconnected handle
// ==================================================================================================

TEST_CASE("catalogTables: every mode refuses with 08003 when there is no live connection") {
  Connection conn;
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(S("tpch"), S("tiny"), S("%"))); }),
            sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(S("%"), S(""), S(""))); }), sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(S(""), S("%"), S(""))); }), sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(S("tpch"), S("%"), S(""))); }), sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(S(""), S(""), S(""), std::nullopt, S("%"))); }),
            sqlstate::kNotConnected);
  // A connected flag without a client object is still "not connected", never a null dereference.
  conn.connected = true;
  CHECK_STR(thrownSqlstate([&] { catalogTables(conn, makeArgs(std::nullopt, std::nullopt, std::nullopt)); }),
            sqlstate::kNotConnected);
}

TEST_CASE("catalogColumns / catalogProcedures / catalogProcedureColumns: 08003 without a live connection") {
  Connection conn;
  CHECK_STR(thrownSqlstate([&] { catalogColumns(conn, makeArgs(S("tpch"), S("tiny"), S("orders"), S("%"))); }),
            sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogProcedures(conn, makeArgs(std::nullopt, std::nullopt, std::nullopt)); }),
            sqlstate::kNotConnected);
  CHECK_STR(thrownSqlstate([&] { catalogProcedureColumns(conn, makeArgs(std::nullopt, std::nullopt, std::nullopt)); }),
            sqlstate::kNotConnected);
}
