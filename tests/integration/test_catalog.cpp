// Catalog-function tests: SQLTables in every form (special enumerations, patterns, escapes, table
// type lists), SQLColumns layout and type-derived columns, the always-empty key/index/privilege
// functions, SQL_ATTR_METADATA_ID literal behaviour, ANSI and Unicode entry points.
#include "integration/odbc_loader.h"

#include <cstring>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// A nullable catalog-function argument in both encodings (nullptr = SQL NULL pointer).
struct Arg {
  bool present;
  std::vector<SQLWCHAR> w;
  std::string a;
  explicit Arg(const char* s) : present(s != nullptr) {
    if (s) {
      w = toWide(s);
      a = s;
    }
  }
  SQLWCHAR* wptr() { return present ? w.data() : nullptr; }
  SQLSMALLINT wlen() const { return present ? static_cast<SQLSMALLINT>(w.size() - 1) : 0; }
  SQLCHAR* aptr() { return present ? narrow(a) : nullptr; }
  SQLSMALLINT alen() const { return present ? static_cast<SQLSMALLINT>(a.size()) : 0; }
};

SQLRETURN tablesW(SQLHSTMT stmt, const char* cat, const char* schem, const char* table, const char* types) {
  Arg c(cat), s(schem), t(table), ty(types);
  return Odbc.SQLTablesW(stmt, c.wptr(), c.wlen(), s.wptr(), s.wlen(), t.wptr(), t.wlen(), ty.wptr(), ty.wlen());
}

SQLRETURN tablesA(SQLHSTMT stmt, const char* cat, const char* schem, const char* table, const char* types) {
  Arg c(cat), s(schem), t(table), ty(types);
  return Odbc.SQLTables(stmt, c.aptr(), c.alen(), s.aptr(), s.alen(), t.aptr(), t.alen(), ty.aptr(), ty.alen());
}

SQLRETURN columnsW(SQLHSTMT stmt, const char* cat, const char* schem, const char* table, const char* column) {
  Arg c(cat), s(schem), t(table), co(column);
  return Odbc.SQLColumnsW(stmt, c.wptr(), c.wlen(), s.wptr(), s.wlen(), t.wptr(), t.wlen(), co.wptr(), co.wlen());
}

SQLRETURN columnsA(SQLHSTMT stmt, const char* cat, const char* schem, const char* table, const char* column) {
  Arg c(cat), s(schem), t(table), co(column);
  return Odbc.SQLColumns(stmt, c.aptr(), c.alen(), s.aptr(), s.alen(), t.aptr(), t.alen(), co.aptr(), co.alen());
}

/// Rows of SQLTablesW on a fresh statement (REQUIREs success).
std::vector<std::vector<std::string>> tableRows(SQLHDBC dbc, const char* cat, const char* schem, const char* table,
                                                const char* types) {
  Stmt stmt(dbc);
  ODBC_REQUIRE(tablesW(stmt.h, cat, schem, table, types), SQL_HANDLE_STMT, stmt.h);
  return fetchAll(stmt.h);
}

std::vector<std::vector<std::string>> columnRows(SQLHDBC dbc, const char* cat, const char* schem, const char* table,
                                                 const char* column) {
  Stmt stmt(dbc);
  ODBC_REQUIRE(columnsW(stmt.h, cat, schem, table, column), SQL_HANDLE_STMT, stmt.h);
  return fetchAll(stmt.h);
}

/// Verifies the column names of the open result set.
void checkLayout(SQLHSTMT stmt, const std::vector<const char*>& expected, const char* function) {
  const std::vector<std::string> names = columnNames(stmt);
  testfw::State::get().checks++;
  if (names.size() != expected.size()) {
    testfw::fail(__FILE__, __LINE__,
                 std::string(function) + " returned " + std::to_string(names.size()) + " columns, expected " +
                     std::to_string(expected.size()),
                 false);
    return;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    testfw::State::get().checks++;
    if (names[i] != expected[i]) {
      testfw::fail(__FILE__, __LINE__,
                   std::string(function) + " column " + std::to_string(i + 1) + " is \"" + names[i] + "\", expected \"" +
                       expected[i] + "\"",
                   false);
    }
  }
}

struct TableRow {
  const char* cat;
  const char* schem;
  const char* name;
  const char* type;
};

// Every table the mock lists, in the order SQLTables must return them
// (ORDER BY table_type, table_cat, table_schem, table_name).
const TableRow kAllTables[] = {
    {"mock", "demo", "big_table", "TABLE"},  {"mock", "demo", "types_demo", "TABLE"}, {"tpch", "tiny", "customer", "TABLE"},
    {"tpch", "tiny", "lineitem", "TABLE"},   {"tpch", "tiny", "nation", "TABLE"},     {"tpch", "tiny", "orders", "TABLE"},
    {"tpch", "tiny", "region", "TABLE"},     {"mock", "demo", "demo_view", "VIEW"},
};

/// Compares SQLTables rows against the expected subset (by index into kAllTables).
void checkTables(const std::vector<std::vector<std::string>>& rows, const std::vector<size_t>& expected, const char* what) {
  testfw::State::get().checks++;
  if (rows.size() != expected.size()) {
    std::string got;
    for (const std::vector<std::string>& r : rows) got += (got.empty() ? "" : ", ") + r[0] + "." + r[1] + "." + r[2];
    testfw::fail(__FILE__, __LINE__,
                 std::string(what) + ": " + std::to_string(rows.size()) + " rows, expected " + std::to_string(expected.size()) +
                     " [" + got + "]",
                 false);
    return;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    const TableRow& e = kAllTables[expected[i]];
    const std::vector<std::string>& r = rows[i];
    testfw::State::get().checks++;
    if (r[0] != e.cat || r[1] != e.schem || r[2] != e.name || r[3] != e.type || r[4] != "<NULL>") {
      testfw::fail(__FILE__, __LINE__,
                   std::string(what) + " row " + std::to_string(i + 1) + " = " + r[0] + "." + r[1] + "." + r[2] + " (" + r[3] +
                       ", remarks " + r[4] + "), expected " + e.cat + "." + e.schem + "." + e.name + " (" + e.type + ")",
                   false);
    }
  }
}

const char* catalogName() { return ctx().mock ? "mock" : "tpch"; }
const char* schemaName() { return ctx().mock ? "demo" : "tiny"; }
const char* tableName() { return ctx().mock ? "types_demo" : "customer"; }

const std::vector<const char*> kColumnsLayout = {
    "TABLE_CAT",     "TABLE_SCHEM",   "TABLE_NAME",       "COLUMN_NAME",       "DATA_TYPE",     "TYPE_NAME",
    "COLUMN_SIZE",   "BUFFER_LENGTH", "DECIMAL_DIGITS",   "NUM_PREC_RADIX",    "NULLABLE",      "REMARKS",
    "COLUMN_DEF",    "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE",
};

}  // namespace

TEST_CASE("catalog/tables_layout_and_all_rows") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(tablesW(stmt.h, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h);
  checkLayout(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"}, "SQLTables");
  for (SQLUSMALLINT c = 1; c <= 5; ++c) {
    const ColumnDesc d = describeCol(stmt.h, c);
    CHECK(d.type == SQL_WVARCHAR || d.type == SQL_VARCHAR);
    CHECK_EQ(d.nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE));
  }
  CHECK_EQ(describeCol(stmt.h, 1).size, static_cast<SQLULEN>(256));
  CHECK_EQ(describeCol(stmt.h, 4).size, static_cast<SQLULEN>(32));
  CHECK_EQ(describeCol(stmt.h, 5).size, static_cast<SQLULEN>(4096));
  SQLLEN rowCount = 0;
  ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rowCount), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));
  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  if (ctx().mock) {
    checkTables(rows, {0, 1, 2, 3, 4, 5, 6, 7}, "SQLTables(NULL,NULL,NULL,NULL)");
  } else {
    bool sawCustomer = false;
    for (const std::vector<std::string>& r : rows) {
      if (r[0] == "tpch" && r[1] == "tiny" && r[2] == "customer" && r[3] == "TABLE") sawCustomer = true;
    }
    CHECK(sawCustomer);
  }
  // The cursor is a normal one: closing and reusing the statement works.
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
}

TEST_CASE("catalog/tables_special_enumerations") {
  Session s;
  // SQL_ALL_CATALOGS: only TABLE_CAT is filled.
  const std::vector<std::vector<std::string>> catalogs = tableRows(s.h(), SQL_ALL_CATALOGS, "", "", nullptr);
  REQUIRE(!catalogs.empty());
  bool sawTpch = false, sawSystem = false;
  for (const std::vector<std::string>& r : catalogs) {
    CHECK_EQ(r[1], std::string("<NULL>"));
    CHECK_EQ(r[2], std::string("<NULL>"));
    CHECK_EQ(r[3], std::string("<NULL>"));
    if (r[0] == "tpch") sawTpch = true;
    if (r[0] == "system") sawSystem = true;
  }
  CHECK(sawTpch);
  CHECK(sawSystem);
  if (ctx().mock) {
    REQUIRE_EQ(catalogs.size(), static_cast<size_t>(3));
    CHECK_EQ(catalogs[0][0], std::string("mock"));
    CHECK_EQ(catalogs[1][0], std::string("system"));
    CHECK_EQ(catalogs[2][0], std::string("tpch"));
  }
  // SQL_ALL_SCHEMAS: distinct schema names, catalog column NULL.
  const std::vector<std::vector<std::string>> schemas = tableRows(s.h(), "", SQL_ALL_SCHEMAS, "", nullptr);
  REQUIRE(!schemas.empty());
  bool sawTiny = false;
  for (size_t i = 0; i < schemas.size(); ++i) {
    CHECK_EQ(schemas[i][0], std::string("<NULL>"));
    CHECK_EQ(schemas[i][2], std::string("<NULL>"));
    if (schemas[i][1] == "tiny") sawTiny = true;
    if (i > 0) CHECK(schemas[i - 1][1] < schemas[i][1]);  // sorted, no duplicates
  }
  CHECK(sawTiny);
  if (ctx().mock) {
    const char* expected[] = {"demo", "information_schema", "jdbc", "runtime", "sf1", "tiny"};
    REQUIRE_EQ(schemas.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < 6; ++i) CHECK_EQ(schemas[i][1], std::string(expected[i]));
  }
  // SQL_ALL_TABLE_TYPES.
  const std::vector<std::vector<std::string>> types = tableRows(s.h(), "", "", "", SQL_ALL_TABLE_TYPES);
  REQUIRE(types.size() >= 2);
  bool sawTable = false, sawView = false;
  for (const std::vector<std::string>& r : types) {
    CHECK_EQ(r[0], std::string("<NULL>"));
    CHECK_EQ(r[2], std::string("<NULL>"));
    if (r[3] == "TABLE") sawTable = true;
    if (r[3] == "VIEW") sawView = true;
  }
  CHECK(sawTable);
  CHECK(sawView);
  if (ctx().mock) CHECK_EQ(types.size(), static_cast<size_t>(2));
  // Schemas of one catalog (lenient form: TABLE_CAT reported).
  const std::vector<std::vector<std::string>> tpchSchemas = tableRows(s.h(), "tpch", SQL_ALL_SCHEMAS, "", nullptr);
  REQUIRE(!tpchSchemas.empty());
  bool sawTpchTiny = false;
  for (const std::vector<std::string>& r : tpchSchemas) {
    CHECK_EQ(r[0], std::string("tpch"));
    CHECK_EQ(r[2], std::string("<NULL>"));
    if (r[1] == "tiny") sawTpchTiny = true;
  }
  CHECK(sawTpchTiny);
  if (ctx().mock) {
    REQUIRE_EQ(tpchSchemas.size(), static_cast<size_t>(3));
    CHECK_EQ(tpchSchemas[0][1], std::string("information_schema"));
    CHECK_EQ(tpchSchemas[1][1], std::string("sf1"));
    CHECK_EQ(tpchSchemas[2][1], std::string("tiny"));
  }
}

TEST_CASE("catalog/tables_filters_and_patterns") {
  Session s;
  // Catalog only.
  const std::vector<std::vector<std::string>> tpch = tableRows(s.h(), "tpch", nullptr, nullptr, nullptr);
  bool sawCustomer = false;
  for (const std::vector<std::string>& r : tpch) {
    CHECK_EQ(r[0], std::string("tpch"));
    if (r[1] == "tiny" && r[2] == "customer") sawCustomer = true;
  }
  CHECK(sawCustomer);
  // Exact table.
  const std::vector<std::vector<std::string>> customer = tableRows(s.h(), "tpch", "tiny", "customer", nullptr);
  REQUIRE_EQ(customer.size(), static_cast<size_t>(1));
  CHECK_EQ(customer[0][2], std::string("customer"));
  CHECK_EQ(customer[0][3], std::string("TABLE"));
  // Patterns: '%' and '_' wildcards.
  const std::vector<std::vector<std::string>> cust = tableRows(s.h(), "tpch", "tiny", "cust%", nullptr);
  REQUIRE_EQ(cust.size(), static_cast<size_t>(1));
  CHECK_EQ(cust[0][2], std::string("customer"));
  const std::vector<std::vector<std::string>> underscore = tableRows(s.h(), "tpch", "t_ny", "nation", nullptr);
  REQUIRE_EQ(underscore.size(), static_cast<size_t>(1));
  CHECK_EQ(underscore[0][1], std::string("tiny"));
  // An escaped wildcard is literal: no table is called "100%".
  CHECK_EQ(tableRows(s.h(), "tpch", "tiny", "100\\%", nullptr).size(), static_cast<size_t>(0));
  // An empty (non-NULL) pattern matches nothing: every Trino table has a schema.
  CHECK_EQ(tableRows(s.h(), "tpch", "", "customer", nullptr).size(), static_cast<size_t>(0));
  if (!ctx().mock) return;
  checkTables(tableRows(s.h(), "tpch", nullptr, nullptr, nullptr), {2, 3, 4, 5, 6}, "SQLTables(tpch)");
  checkTables(tableRows(s.h(), nullptr, nullptr, "%_table", nullptr), {0}, "SQLTables(%_table)");
  checkTables(tableRows(s.h(), "mock", "demo", "types\\_demo", nullptr), {1}, "SQLTables(types\\_demo)");
  checkTables(tableRows(s.h(), "mock", "demo", "types_demo", nullptr), {1}, "SQLTables(types_demo)");
  checkTables(tableRows(s.h(), "%", "%", "%", nullptr), {0, 1, 2, 3, 4, 5, 6, 7}, "SQLTables(%,%,%)");
  checkTables(tableRows(s.h(), "m%", "d%", "%", nullptr), {0, 1, 7}, "SQLTables(m%,d%,%)");
  checkTables(tableRows(s.h(), nullptr, "tiny", "%n", nullptr), {4, 6}, "SQLTables(tiny,%n)");
}

TEST_CASE("catalog/tables_type_filter") {
  Session s;
  if (!ctx().mock) {
    const std::vector<std::vector<std::string>> rows = tableRows(s.h(), "tpch", "tiny", "%", "TABLE,VIEW");
    CHECK(!rows.empty());
    for (const std::vector<std::string>& r : rows) CHECK(r[3] == "TABLE" || r[3] == "VIEW");
    CHECK_EQ(tableRows(s.h(), "tpch", "tiny", "%", "SYSTEM TABLE").size(), static_cast<size_t>(0));
    return;
  }
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "TABLE,VIEW"), {0, 1, 2, 3, 4, 5, 6, 7}, "TABLE,VIEW");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "'TABLE'"), {0, 1, 2, 3, 4, 5, 6}, "'TABLE'");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "'TABLE','VIEW'"), {0, 1, 2, 3, 4, 5, 6, 7}, "'TABLE','VIEW'");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "VIEW"), {7}, "VIEW");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "table"), {0, 1, 2, 3, 4, 5, 6}, "table (lower case)");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, " VIEW , TABLE "), {0, 1, 2, 3, 4, 5, 6, 7}, "spaced list");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, "SYSTEM TABLE"), {}, "SYSTEM TABLE");
  checkTables(tableRows(s.h(), nullptr, nullptr, nullptr, ""), {0, 1, 2, 3, 4, 5, 6, 7}, "empty type list");
  checkTables(tableRows(s.h(), "mock", "demo", "%", "TABLE,VIEW"), {0, 1, 7}, "mock.demo TABLE,VIEW");
  checkTables(tableRows(s.h(), "mock", "demo", "%", "TABLE"), {0, 1}, "mock.demo TABLE");
}

TEST_CASE("catalog/tables_ansi_entry_point") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(tablesA(stmt.h, "tpch", "tiny", "cust%", nullptr), SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(1));
  CHECK_EQ(rows[0][2], std::string("customer"));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(tablesA(stmt.h, SQL_ALL_CATALOGS, "", "", nullptr), SQL_HANDLE_STMT, stmt.h);
  CHECK(countRows(stmt.h) >= 2);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(columnsA(stmt.h, "tpch", "tiny", "customer", "c%"), SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::vector<std::string>> cols = fetchAll(stmt.h);
  REQUIRE_EQ(cols.size(), static_cast<size_t>(2));  // custkey, comment
  CHECK_EQ(cols[0][3], std::string("custkey"));
  CHECK_EQ(cols[1][3], std::string("comment"));
}

TEST_CASE("catalog/columns_layout_and_customer_rows") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(columnsW(stmt.h, "tpch", "tiny", "customer", nullptr), SQL_HANDLE_STMT, stmt.h);
  checkLayout(stmt.h, kColumnsLayout, "SQLColumns");
  // Column types of the result set itself.
  CHECK_EQ(describeCol(stmt.h, 5).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));   // DATA_TYPE
  CHECK_EQ(describeCol(stmt.h, 7).type, static_cast<SQLSMALLINT>(SQL_INTEGER));    // COLUMN_SIZE
  CHECK_EQ(describeCol(stmt.h, 8).type, static_cast<SQLSMALLINT>(SQL_INTEGER));    // BUFFER_LENGTH
  CHECK_EQ(describeCol(stmt.h, 9).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));   // DECIMAL_DIGITS
  CHECK_EQ(describeCol(stmt.h, 10).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));  // NUM_PREC_RADIX
  CHECK_EQ(describeCol(stmt.h, 11).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));  // NULLABLE
  CHECK_EQ(describeCol(stmt.h, 14).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));  // SQL_DATA_TYPE
  CHECK_EQ(describeCol(stmt.h, 15).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));  // SQL_DATETIME_SUB
  CHECK_EQ(describeCol(stmt.h, 16).type, static_cast<SQLSMALLINT>(SQL_INTEGER));   // CHAR_OCTET_LENGTH
  CHECK_EQ(describeCol(stmt.h, 17).type, static_cast<SQLSMALLINT>(SQL_INTEGER));   // ORDINAL_POSITION
  CHECK_EQ(describeCol(stmt.h, 3).nullable, static_cast<SQLSMALLINT>(SQL_NO_NULLS));   // TABLE_NAME
  CHECK_EQ(describeCol(stmt.h, 5).nullable, static_cast<SQLSMALLINT>(SQL_NO_NULLS));   // DATA_TYPE
  CHECK_EQ(describeCol(stmt.h, 12).nullable, static_cast<SQLSMALLINT>(SQL_NULLABLE));  // REMARKS
  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(8));
  const char* names[] = {"custkey", "name", "address", "nationkey", "phone", "acctbal", "mktsegment", "comment"};
  for (size_t i = 0; i < 8; ++i) {
    const std::vector<std::string>& r = rows[i];
    CHECK_EQ(r[0], std::string("tpch"));
    CHECK_EQ(r[1], std::string("tiny"));
    CHECK_EQ(r[2], std::string("customer"));
    CHECK_EQ(r[3], std::string(names[i]));
    CHECK_EQ(r[16], std::to_string(i + 1));
    CHECK(r[17] == "YES" || r[17] == "NO" || r[17].empty());
    CHECK_EQ(r[13] == "<NULL>" ? "missing" : "ok", std::string("ok"));  // SQL_DATA_TYPE never NULL
  }
  // custkey bigint.
  CHECK_EQ(rows[0][4], std::to_string(SQL_BIGINT));
  CHECK_EQ(rows[0][5], std::string("bigint"));
  CHECK_EQ(rows[0][6], std::string("19"));
  CHECK_EQ(rows[0][7], std::string("8"));
  CHECK_EQ(rows[0][8], std::string("0"));
  CHECK_EQ(rows[0][9], std::string("10"));
  CHECK_EQ(rows[0][13], std::to_string(SQL_BIGINT));
  CHECK_EQ(rows[0][14], std::string("<NULL>"));
  CHECK_EQ(rows[0][15], std::string("<NULL>"));
  // name varchar(25): wide types by default, octet lengths in bytes of SQLWCHAR data (2 per char).
  CHECK_EQ(rows[1][4], std::to_string(SQL_WVARCHAR));
  CHECK_EQ(rows[1][5], std::string("varchar(25)"));
  CHECK_EQ(rows[1][6], std::string("25"));
  CHECK_EQ(rows[1][7], std::to_string(25 * sizeof(SQLWCHAR)));
  CHECK_EQ(rows[1][8], std::string("<NULL>"));
  CHECK_EQ(rows[1][9], std::string("<NULL>"));
  CHECK_EQ(rows[1][13], std::to_string(SQL_WVARCHAR));
  CHECK_EQ(rows[1][15], std::to_string(25 * sizeof(SQLWCHAR)));
  // acctbal double.
  CHECK_EQ(rows[5][4], std::to_string(SQL_DOUBLE));
  CHECK_EQ(rows[5][5], std::string("double"));
  CHECK_EQ(rows[5][6], std::string("15"));
  CHECK_EQ(rows[5][7], std::string("8"));
  CHECK_EQ(rows[5][8], std::string("<NULL>"));
  CHECK_EQ(rows[5][9], std::string("10"));  // 15 decimal digits, so radix 10 (review 71)
  CHECK_EQ(rows[5][15], std::string("<NULL>"));
  if (ctx().mock) {
    CHECK_EQ(rows[0][10], std::to_string(SQL_NULLABLE));
    CHECK_EQ(rows[0][17], std::string("YES"));
    CHECK_EQ(rows[0][11], std::string("<NULL>"));  // REMARKS
    CHECK_EQ(rows[0][12], std::string("<NULL>"));  // COLUMN_DEF
  }
}

TEST_CASE("catalog/columns_type_derived_values") {
  HARNESS_MOCK_ONLY();
  Session s;
  const std::vector<std::vector<std::string>> rows = columnRows(s.h(), "mock", "demo", "types_demo", nullptr);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(26));
  auto row = [&](const char* column) -> const std::vector<std::string>& {
    for (const std::vector<std::string>& r : rows) {
      if (r[3] == column) return r;
    }
    throw testfw::Failure(std::string("column ") + column + " missing from SQLColumns");
  };
  const std::vector<std::string>& date = row("c_date");
  CHECK_EQ(date[4], std::to_string(SQL_TYPE_DATE));
  CHECK_EQ(date[6], std::string("10"));
  CHECK_EQ(date[7], std::to_string(sizeof(SQL_DATE_STRUCT)));
  CHECK_EQ(date[8], std::string("<NULL>"));
  CHECK_EQ(date[13], std::to_string(SQL_DATETIME));
  CHECK_EQ(date[14], std::to_string(SQL_CODE_DATE));
  const std::vector<std::string>& ts = row("c_timestamp3");
  CHECK_EQ(ts[4], std::to_string(SQL_TYPE_TIMESTAMP));
  CHECK_EQ(ts[5], std::string("timestamp(3)"));
  CHECK_EQ(ts[6], std::string("23"));
  CHECK_EQ(ts[8], std::string("3"));
  CHECK_EQ(ts[9], std::string("<NULL>"));
  CHECK_EQ(ts[13], std::to_string(SQL_DATETIME));
  CHECK_EQ(ts[14], std::to_string(SQL_CODE_TIMESTAMP));
  const std::vector<std::string>& time = row("c_time3");
  CHECK_EQ(time[4], std::to_string(SQL_TYPE_TIME));
  CHECK_EQ(time[6], std::string("12"));
  CHECK_EQ(time[8], std::string("3"));
  CHECK_EQ(time[14], std::to_string(SQL_CODE_TIME));
  const std::vector<std::string>& dec = row("c_decimal_10_2");
  CHECK_EQ(dec[4], std::to_string(SQL_DECIMAL));
  CHECK_EQ(dec[5], std::string("decimal(10,2)"));
  CHECK_EQ(dec[6], std::string("10"));
  CHECK_EQ(dec[7], std::string("12"));
  CHECK_EQ(dec[8], std::string("2"));
  CHECK_EQ(dec[9], std::string("10"));
  CHECK_EQ(dec[13], std::to_string(SQL_DECIMAL));
  CHECK_EQ(dec[14], std::string("<NULL>"));
  const std::vector<std::string>& vc = row("c_varchar20");
  CHECK_EQ(vc[4], std::to_string(SQL_WVARCHAR));
  CHECK_EQ(vc[6], std::string("20"));
  CHECK_EQ(vc[7], std::to_string(20 * sizeof(SQLWCHAR)));
  CHECK_EQ(vc[15], std::to_string(20 * sizeof(SQLWCHAR)));
  const std::vector<std::string>& unbounded = row("c_varchar");
  CHECK_EQ(unbounded[5], std::string("varchar"));
  CHECK_EQ(unbounded[6], std::string("65535"));
  const std::vector<std::string>& bin = row("c_varbinary");
  CHECK_EQ(bin[4], std::to_string(SQL_VARBINARY));
  CHECK_EQ(bin[15], std::string("65535"));
  const std::vector<std::string>& boolean = row("c_boolean");
  CHECK_EQ(boolean[4], std::to_string(SQL_BIT));
  CHECK_EQ(boolean[6], std::string("1"));
  CHECK_EQ(boolean[8], std::string("0"));
  const std::vector<std::string>& tz = row("c_timestamp6_tz");
  CHECK_EQ(tz[4], std::to_string(SQL_TYPE_TIMESTAMP));
  CHECK_EQ(tz[5], std::string("timestamp(6) with time zone"));
  CHECK_EQ(tz[6], std::string("26"));
  CHECK_EQ(tz[8], std::string("6"));
  const std::vector<std::string>& arr = row("c_array");
  CHECK_EQ(arr[4], std::to_string(SQL_WLONGVARCHAR));
  CHECK_EQ(arr[5], std::string("array(integer)"));
  const std::vector<std::string>& uuid = row("c_uuid");
  CHECK_EQ(uuid[4], std::to_string(SQL_WVARCHAR));
  CHECK_EQ(uuid[6], std::string("36"));
  // Intervals exposed as text (default IntervalMode=string) carry no SQL_DATETIME_SUB (review 38).
  for (const char* name : {"c_interval_ym", "c_interval_ds"}) {
    const std::vector<std::string>& iv = row(name);
    CHECK_EQ(iv[4], std::to_string(SQL_WVARCHAR));
    CHECK_EQ(iv[13], std::to_string(SQL_WVARCHAR));
    CHECK_EQ(iv[14], std::string("<NULL>"));
  }
  Session intervals("IntervalMode=interval");
  const std::vector<std::vector<std::string>> irows =
      columnRows(intervals.h(), "mock", "demo", "types_demo", "c_interval%");
  REQUIRE_EQ(irows.size(), static_cast<size_t>(2));
  for (const std::vector<std::string>& r : irows) {
    const bool ym = r[3] == "c_interval_ym";
    CHECK_EQ(r[4], std::to_string(ym ? SQL_INTERVAL_YEAR_TO_MONTH : SQL_INTERVAL_DAY_TO_SECOND));
    CHECK_EQ(r[13], std::to_string(SQL_INTERVAL));
    CHECK_EQ(r[14], std::to_string(ym ? SQL_CODE_YEAR_TO_MONTH : SQL_CODE_DAY_TO_SECOND));
  }
  // Connection options change what SQLColumns reports, consistently with SQLDescribeCol.
  Session ansi("UseUnicodeSqlCharacterTypes=0;DefaultStringColumnLength=4000;UuidAsGuid=1");
  const std::vector<std::vector<std::string>> arows = columnRows(ansi.h(), "mock", "demo", "types_demo", "c_varchar%");
  REQUIRE_EQ(arows.size(), static_cast<size_t>(2));
  CHECK_EQ(arows[0][3], std::string("c_varchar"));
  CHECK_EQ(arows[0][4], std::to_string(SQL_VARCHAR));
  CHECK_EQ(arows[0][6], std::string("4000"));
  CHECK_EQ(arows[0][7], std::string("4000"));
  CHECK_EQ(arows[1][3], std::string("c_varchar20"));
  CHECK_EQ(arows[1][7], std::string("20"));
  const std::vector<std::vector<std::string>> guid = columnRows(ansi.h(), "mock", "demo", "types_demo", "c_uuid");
  REQUIRE_EQ(guid.size(), static_cast<size_t>(1));
  CHECK_EQ(guid[0][4], std::to_string(SQL_GUID));
  CHECK_EQ(guid[0][7], std::to_string(sizeof(SQLGUID)));
}

TEST_CASE("catalog/columns_patterns_and_null_arguments") {
  Session s;
  // Column pattern with an escaped underscore.
  const std::vector<std::vector<std::string>> pattern =
      columnRows(s.h(), catalogName(), schemaName(), tableName(), ctx().mock ? "c\\_dec%" : "c%");
  if (ctx().mock) {
    REQUIRE_EQ(pattern.size(), static_cast<size_t>(2));
    CHECK_EQ(pattern[0][3], std::string("c_decimal_10_2"));
    CHECK_EQ(pattern[1][3], std::string("c_decimal_38_10"));
  } else {
    REQUIRE_EQ(pattern.size(), static_cast<size_t>(2));  // custkey, comment
  }
  // "%" and NULL both mean "every column".
  const std::vector<std::vector<std::string>> all = columnRows(s.h(), catalogName(), schemaName(), tableName(), "%");
  const std::vector<std::vector<std::string>> nullArg = columnRows(s.h(), catalogName(), schemaName(), tableName(), nullptr);
  CHECK_EQ(all.size(), nullArg.size());
  CHECK_EQ(all.size(), static_cast<size_t>(ctx().mock ? 26 : 8));
  // Unknown table: empty result, not an error.
  CHECK_EQ(columnRows(s.h(), catalogName(), schemaName(), "no_such_table_xyz", nullptr).size(), static_cast<size_t>(0));
  if (!ctx().mock) return;
  // Every column of the schema, ordered by table then ordinal position.
  const std::vector<std::vector<std::string>> schema = columnRows(s.h(), "mock", "demo", nullptr, nullptr);
  REQUIRE_EQ(schema.size(), static_cast<size_t>(5 + 2 + 26));
  CHECK_EQ(schema[0][2], std::string("big_table"));
  CHECK_EQ(schema[0][3], std::string("id"));
  CHECK_EQ(schema[4][3], std::string("amount"));
  CHECK_EQ(schema[5][2], std::string("demo_view"));
  CHECK_EQ(schema[7][2], std::string("types_demo"));
  CHECK_EQ(schema[32][3], std::string("c_ipaddress"));
  CHECK_EQ(schema[32][16], std::string("26"));
  // Table name patterns work for SQLColumns as well.
  const std::vector<std::vector<std::string>> demoTables = columnRows(s.h(), "mock", "demo", "%_view", nullptr);
  REQUIRE_EQ(demoTables.size(), static_cast<size_t>(2));
  CHECK_EQ(demoTables[0][3], std::string("id"));
  CHECK_EQ(demoTables[1][3], std::string("label"));
  // SQLColumns' CatalogName is an ordinary argument, not a pattern: "m%" names no catalog.
  CHECK_EQ(columnRows(s.h(), "m%", "demo", "types_demo", nullptr).size(), static_cast<size_t>(0));
}

TEST_CASE("catalog/metadata_id_literal_identifiers") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_METADATA_ID, attrVal(SQL_TRUE), 0), SQL_HANDLE_STMT, stmt.h);
  // Literal names match exactly; wildcards and escapes are ordinary characters now.
  ODBC_REQUIRE(tablesW(stmt.h, "mock", "demo", "types_demo", nullptr), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(tablesW(stmt.h, "mock", "demo", "types\\_demo", nullptr), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(tablesW(stmt.h, "mock", "demo", "types%", nullptr), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  // Quoted identifiers are unquoted; trailing blanks are ignored.
  ODBC_REQUIRE(tablesW(stmt.h, "\"mock\"", "demo  ", "\"types_demo\"", nullptr), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(columnsW(stmt.h, "mock", "demo", "types_demo", "c_date"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(1));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(columnsW(stmt.h, "mock", "demo", "types_demo", "c_d%"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  // Back to pattern mode on the same statement.
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_METADATA_ID, attrVal(SQL_FALSE), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(columnsW(stmt.h, "mock", "demo", "types_demo", "c_d%"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(4));  // c_double, c_decimal_10_2, c_decimal_38_10, c_date
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
  // The connection option makes literal matching the default for every statement.
  Session literal("MetadataCaseSensitive=1");
  CHECK_EQ(tableRows(literal.h(), "mock", "demo", "types%", nullptr).size(), static_cast<size_t>(0));
  CHECK_EQ(tableRows(literal.h(), "mock", "demo", "types_demo", nullptr).size(), static_cast<size_t>(1));
  // Simba-style alias of the same option.
  Session alias("AssumeLiteralNamesInMetadataCalls=1");
  CHECK_EQ(tableRows(alias.h(), "mock", "demo", "big%", nullptr).size(), static_cast<size_t>(0));
}

TEST_CASE("catalog/empty_result_functions_have_spec_layouts") {
  Session s;
  Arg cat(catalogName()), schem(schemaName()), table(tableName());
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLPrimaryKeysW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen()),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"}, "SQLPrimaryKeys");
    CHECK_EQ(describeCol(stmt.h, 5).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));
    CHECK_EQ(describeCol(stmt.h, 3).nullable, static_cast<SQLSMALLINT>(SQL_NO_NULLS));
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
    // ANSI variant.
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(Odbc.SQLPrimaryKeys(stmt.h, cat.aptr(), cat.alen(), schem.aptr(), schem.alen(), table.aptr(), table.alen()),
                 SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(numResultCols(stmt.h), static_cast<SQLSMALLINT>(6));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLForeignKeysW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(),
                                      nullptr, 0, nullptr, 0, nullptr, 0),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h,
                {"PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME", "FKTABLE_CAT", "FKTABLE_SCHEM", "FKTABLE_NAME",
                 "FKCOLUMN_NAME", "KEY_SEQ", "UPDATE_RULE", "DELETE_RULE", "FK_NAME", "PK_NAME", "DEFERRABILITY"},
                "SQLForeignKeys");
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLStatisticsW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(),
                                     SQL_INDEX_ALL, SQL_QUICK),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h,
                {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "NON_UNIQUE", "INDEX_QUALIFIER", "INDEX_NAME", "TYPE",
                 "ORDINAL_POSITION", "COLUMN_NAME", "ASC_OR_DESC", "CARDINALITY", "PAGES", "FILTER_CONDITION"},
                "SQLStatistics");
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK_ERROR(Odbc.SQLStatisticsW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(), 99, SQL_QUICK),
                     SQL_HANDLE_STMT, stmt.h, "HY100");
    ODBC_CHECK_ERROR(Odbc.SQLStatisticsW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(), SQL_INDEX_UNIQUE, 99),
                     SQL_HANDLE_STMT, stmt.h, "HY101");
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLSpecialColumnsW(stmt.h, SQL_BEST_ROWID, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(),
                                         table.wlen(), SQL_SCOPE_CURROW, SQL_NULLABLE),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h, {"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"},
                "SQLSpecialColumns");
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
    ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_CLOSE), SQL_HANDLE_STMT, stmt.h);
    ODBC_CHECK_ERROR(Odbc.SQLSpecialColumnsW(stmt.h, 99, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(),
                                             SQL_SCOPE_CURROW, SQL_NULLABLE),
                     SQL_HANDLE_STMT, stmt.h, "HY097");
    ODBC_CHECK_ERROR(Odbc.SQLSpecialColumnsW(stmt.h, SQL_ROWVER, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(),
                                             table.wlen(), 99, SQL_NULLABLE),
                     SQL_HANDLE_STMT, stmt.h, "HY098");
    ODBC_CHECK_ERROR(Odbc.SQLSpecialColumnsW(stmt.h, SQL_ROWVER, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(),
                                             table.wlen(), SQL_SCOPE_CURROW, 99),
                     SQL_HANDLE_STMT, stmt.h, "HY099");
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLProceduresW(stmt.h, cat.wptr(), cat.wlen(), nullptr, 0, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h,
                {"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "NUM_INPUT_PARAMS", "NUM_OUTPUT_PARAMS", "NUM_RESULT_SETS",
                 "REMARKS", "PROCEDURE_TYPE"},
                "SQLProcedures");
    if (ctx().mock) CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLProcedureColumnsW(stmt.h, cat.wptr(), cat.wlen(), nullptr, 0, nullptr, 0, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h,
                {"PROCEDURE_CAT", "PROCEDURE_SCHEM", "PROCEDURE_NAME", "COLUMN_NAME", "COLUMN_TYPE", "DATA_TYPE", "TYPE_NAME",
                 "COLUMN_SIZE", "BUFFER_LENGTH", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "NULLABLE", "REMARKS", "COLUMN_DEF",
                 "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE"},
                "SQLProcedureColumns");
    if (ctx().mock) CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLTablePrivilegesW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen()),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "GRANTOR", "GRANTEE", "PRIVILEGE", "IS_GRANTABLE"},
                "SQLTablePrivileges");
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLColumnPrivilegesW(stmt.h, cat.wptr(), cat.wlen(), schem.wptr(), schem.wlen(), table.wptr(), table.wlen(),
                                           nullptr, 0),
                 SQL_HANDLE_STMT, stmt.h);
    checkLayout(stmt.h, {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "GRANTOR", "GRANTEE", "PRIVILEGE", "IS_GRANTABLE"},
                "SQLColumnPrivileges");
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(0));
    // The empty result is a real cursor: fetching past the end and closing behave normally.
    CHECK(Odbc.SQLFetch(stmt.h) == SQL_NO_DATA);
    ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  }
}

TEST_CASE("catalog/functions_require_closed_cursor") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(tablesW(stmt.h, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(columnsW(stmt.h, nullptr, nullptr, nullptr, nullptr), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK_ERROR(Odbc.SQLGetTypeInfoW(stmt.h, SQL_ALL_TYPES), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // A catalog result replaces a finished DML result on the same statement.
  if (ctx().mock) {
    ODBC_REQUIRE(execDirect(stmt.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, stmt.h);
    ODBC_REQUIRE(tablesW(stmt.h, SQL_ALL_CATALOGS, "", "", nullptr), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(3));
    SQLLEN rowCount = 0;
    ODBC_CHECK(Odbc.SQLRowCount(stmt.h, &rowCount), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(rowCount, static_cast<SQLLEN>(-1));
  }
  // Unicode arguments survive the round trip (no rows, but no error either).
  Stmt uni(s.dbc);
  ODBC_REQUIRE(tablesW(uni.h, nullptr, nullptr, "tablo_ğüş_日本", nullptr), SQL_HANDLE_STMT, uni.h);
  CHECK_EQ(countRows(uni.h), static_cast<int64_t>(0));
  // A quote inside a pattern is escaped correctly in the generated SQL.
  ODBC_CHECK(Odbc.SQLFreeStmt(uni.h, SQL_CLOSE), SQL_HANDLE_STMT, uni.h);
  ODBC_REQUIRE(tablesW(uni.h, nullptr, nullptr, "it's%", nullptr), SQL_HANDLE_STMT, uni.h);
  CHECK_EQ(countRows(uni.h), static_cast<int64_t>(0));
}

TEST_CASE("catalog/show_statements_as_result_sets") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SHOW CATALOGS"), SQL_HANDLE_STMT, stmt.h);
  const std::string firstName = columnNames(stmt.h)[0];
  CHECK_EQ(firstName, std::string("Catalog"));
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(3));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SHOW SCHEMAS FROM tpch"), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(countRows(stmt.h), static_cast<int64_t>(3));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SHOW TABLES FROM tpch.tiny"), SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  REQUIRE_EQ(rows.size(), static_cast<size_t>(5));
  CHECK_EQ(rows[0][0], std::string("customer"));
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  // Direct system.jdbc access is an ordinary query too.
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT table_name FROM system.jdbc.tables WHERE table_schem = 'demo' ORDER BY table_name"),
               SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::vector<std::string>> jdbc = fetchAll(stmt.h);
  REQUIRE_EQ(jdbc.size(), static_cast<size_t>(3));
  CHECK_EQ(jdbc[0][0], std::string("big_table"));
  CHECK_EQ(jdbc[1][0], std::string("demo_view"));
  CHECK_EQ(jdbc[2][0], std::string("types_demo"));
}
