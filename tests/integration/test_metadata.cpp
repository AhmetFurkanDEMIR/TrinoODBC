// Metadata tests: SQLGetInfo, SQLGetFunctions, SQLGetTypeInfo, attribute round trips, descriptors,
// SQLNativeSql and cursor names.
#include "integration/odbc_loader.h"

#include <cctype>
#include <cstring>
#include <set>
#include <string>

using namespace harness;

namespace {

struct StringExpectation {
  SQLUSMALLINT type;
  const char* name;
  const char* expected;  // nullptr = only require success
};

struct UIntExpectation {
  SQLUSMALLINT type;
  const char* name;
  SQLUINTEGER mustHave;   // bits that must be set (0 = none)
  SQLUINTEGER exact;      // exact value when `isExact`
  bool isExact;
};

struct USmallExpectation {
  SQLUSMALLINT type;
  const char* name;
  SQLUSMALLINT exact;
};

bool matchesDriverVersion(const std::string& v) {
  // ##.##.####
  if (v.size() != 10 || v[2] != '.' || v[5] != '.') return false;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i == 2 || i == 5) continue;
    if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("metadata/getinfo_strings") {
  Session s;
  const StringExpectation table[] = {
      {SQL_DRIVER_NAME, "SQL_DRIVER_NAME", nullptr},
      {SQL_DRIVER_VER, "SQL_DRIVER_VER", nullptr},
      {SQL_DRIVER_ODBC_VER, "SQL_DRIVER_ODBC_VER", "03.80"},
      {SQL_DBMS_NAME, "SQL_DBMS_NAME", "Trino"},
      {SQL_DBMS_VER, "SQL_DBMS_VER", nullptr},
      {SQL_SERVER_NAME, "SQL_SERVER_NAME", nullptr},
      {SQL_DATA_SOURCE_NAME, "SQL_DATA_SOURCE_NAME", nullptr},
      {SQL_USER_NAME, "SQL_USER_NAME", nullptr},
      {SQL_DATABASE_NAME, "SQL_DATABASE_NAME", nullptr},
      {SQL_CATALOG_NAME, "SQL_CATALOG_NAME", "Y"},
      {SQL_CATALOG_TERM, "SQL_CATALOG_TERM", "catalog"},
      {SQL_SCHEMA_TERM, "SQL_SCHEMA_TERM", "schema"},
      {SQL_TABLE_TERM, "SQL_TABLE_TERM", "table"},
      {SQL_PROCEDURE_TERM, "SQL_PROCEDURE_TERM", "procedure"},
      {SQL_CATALOG_NAME_SEPARATOR, "SQL_CATALOG_NAME_SEPARATOR", "."},
      {SQL_IDENTIFIER_QUOTE_CHAR, "SQL_IDENTIFIER_QUOTE_CHAR", "\""},
      {SQL_SEARCH_PATTERN_ESCAPE, "SQL_SEARCH_PATTERN_ESCAPE", "\\"},
      {SQL_LIKE_ESCAPE_CLAUSE, "SQL_LIKE_ESCAPE_CLAUSE", "Y"},
      {SQL_ACCESSIBLE_TABLES, "SQL_ACCESSIBLE_TABLES", "Y"},
      {SQL_ACCESSIBLE_PROCEDURES, "SQL_ACCESSIBLE_PROCEDURES", "N"},
      {SQL_PROCEDURES, "SQL_PROCEDURES", "N"},
      {SQL_MULT_RESULT_SETS, "SQL_MULT_RESULT_SETS", "N"},
      {SQL_MULTIPLE_ACTIVE_TXN, "SQL_MULTIPLE_ACTIVE_TXN", "N"},
      {SQL_DATA_SOURCE_READ_ONLY, "SQL_DATA_SOURCE_READ_ONLY", "N"},
      {SQL_COLUMN_ALIAS, "SQL_COLUMN_ALIAS", "Y"},
      {SQL_OUTER_JOINS, "SQL_OUTER_JOINS", "Y"},
      {SQL_EXPRESSIONS_IN_ORDERBY, "SQL_EXPRESSIONS_IN_ORDERBY", "Y"},
      {SQL_ORDER_BY_COLUMNS_IN_SELECT, "SQL_ORDER_BY_COLUMNS_IN_SELECT", "N"},
      {SQL_XOPEN_CLI_YEAR, "SQL_XOPEN_CLI_YEAR", "1995"},
      {SQL_DESCRIBE_PARAMETER, "SQL_DESCRIBE_PARAMETER", "Y"},
      {SQL_NEED_LONG_DATA_LEN, "SQL_NEED_LONG_DATA_LEN", "N"},
      {SQL_ROW_UPDATES, "SQL_ROW_UPDATES", "N"},
      {SQL_INTEGRITY, "SQL_INTEGRITY", "N"},
      {SQL_KEYWORDS, "SQL_KEYWORDS", nullptr},
      {SQL_SPECIAL_CHARACTERS, "SQL_SPECIAL_CHARACTERS", ""},
      {SQL_COLLATION_SEQ, "SQL_COLLATION_SEQ", nullptr},
  };
  for (const StringExpectation& e : table) {
    SQLRETURN rc = SQL_ERROR;
    const std::string v = getInfoString(s.h(), e.type, &rc);
    testfw::State::get().checks++;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") failed: " + diagText(SQL_HANDLE_DBC, s.h()), false);
      continue;
    }
    if (e.expected && v != e.expected) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") = \"" + v + "\", expected \"" + e.expected + "\"", false);
    }
  }
  const std::string driverName = getInfoString(s.h(), SQL_DRIVER_NAME);
  CHECK(driverName.find("trinoodbc") != std::string::npos);
  CHECK(matchesDriverVersion(getInfoString(s.h(), SQL_DRIVER_VER)));
  const std::string dbmsVer = getInfoString(s.h(), SQL_DBMS_VER);
  CHECK(!dbmsVer.empty());
  if (ctx().mock) {
    CHECK(dbmsVer.find("mock") != std::string::npos);
    CHECK(dbmsVer.find("483") != std::string::npos);
    CHECK_EQ(getInfoString(s.h(), SQL_SERVER_NAME), ctx().host);
    CHECK_EQ(getInfoString(s.h(), SQL_USER_NAME), std::string("test"));
    CHECK_EQ(getInfoString(s.h(), SQL_DATABASE_NAME), std::string("mock"));
  }
  const std::string keywords = getInfoString(s.h(), SQL_KEYWORDS);
  CHECK(keywords.find("SELECT") != std::string::npos);
  CHECK(keywords.find("CURRENT_CATALOG") != std::string::npos);
}

TEST_CASE("metadata/getinfo_numeric") {
  Session s;
  const USmallExpectation smalls[] = {
      {SQL_MAX_IDENTIFIER_LEN, "SQL_MAX_IDENTIFIER_LEN", 128},
      {SQL_TXN_CAPABLE, "SQL_TXN_CAPABLE", SQL_TC_NONE},
      {SQL_CURSOR_COMMIT_BEHAVIOR, "SQL_CURSOR_COMMIT_BEHAVIOR", SQL_CB_PRESERVE},
      {SQL_CURSOR_ROLLBACK_BEHAVIOR, "SQL_CURSOR_ROLLBACK_BEHAVIOR", SQL_CB_PRESERVE},
      {SQL_CATALOG_LOCATION, "SQL_CATALOG_LOCATION", SQL_CL_START},
      {SQL_IDENTIFIER_CASE, "SQL_IDENTIFIER_CASE", SQL_IC_LOWER},
      {SQL_QUOTED_IDENTIFIER_CASE, "SQL_QUOTED_IDENTIFIER_CASE", SQL_IC_SENSITIVE},
      {SQL_MAX_COLUMNS_IN_SELECT, "SQL_MAX_COLUMNS_IN_SELECT", 0},
      {SQL_MAX_DRIVER_CONNECTIONS, "SQL_MAX_DRIVER_CONNECTIONS", 0},
      {SQL_MAX_CONCURRENT_ACTIVITIES, "SQL_MAX_CONCURRENT_ACTIVITIES", 0},
      {SQL_ACTIVE_ENVIRONMENTS, "SQL_ACTIVE_ENVIRONMENTS", 0},
      {SQL_NULL_COLLATION, "SQL_NULL_COLLATION", SQL_NC_END},  // Trino: NULLS LAST in both directions
      {SQL_CONCAT_NULL_BEHAVIOR, "SQL_CONCAT_NULL_BEHAVIOR", SQL_CB_NULL},
      {SQL_GROUP_BY, "SQL_GROUP_BY", SQL_GB_GROUP_BY_CONTAINS_SELECT},
      {SQL_CORRELATION_NAME, "SQL_CORRELATION_NAME", SQL_CN_ANY},
      {SQL_NON_NULLABLE_COLUMNS, "SQL_NON_NULLABLE_COLUMNS", SQL_NNC_NON_NULL},
      {SQL_FILE_USAGE, "SQL_FILE_USAGE", SQL_FILE_NOT_SUPPORTED},
  };
  for (const USmallExpectation& e : smalls) {
    SQLRETURN rc = SQL_ERROR;
    const SQLUSMALLINT v = getInfoUSmall(s.h(), e.type, &rc);
    testfw::State::get().checks++;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") failed: " + diagText(SQL_HANDLE_DBC, s.h()), false);
    } else if (v != e.exact) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") = " + std::to_string(v) + ", expected " + std::to_string(e.exact), false);
    }
  }
  const UIntExpectation uints[] = {
      {SQL_GETDATA_EXTENSIONS, "SQL_GETDATA_EXTENSIONS", SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK | SQL_GD_BOUND, 0, false},
      {SQL_SCROLL_OPTIONS, "SQL_SCROLL_OPTIONS", 0, SQL_SO_FORWARD_ONLY, true},
      {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, "SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1", SQL_CA1_NEXT, 0, false},
      {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, "SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2", SQL_CA2_READ_ONLY_CONCURRENCY | SQL_CA2_MAX_ROWS_SELECT, 0, false},
      {SQL_STATIC_CURSOR_ATTRIBUTES1, "SQL_STATIC_CURSOR_ATTRIBUTES1", 0, 0, true},
      {SQL_CATALOG_USAGE, "SQL_CATALOG_USAGE", SQL_CU_DML_STATEMENTS | SQL_CU_TABLE_DEFINITION, 0, false},
      {SQL_SCHEMA_USAGE, "SQL_SCHEMA_USAGE", SQL_SU_DML_STATEMENTS | SQL_SU_TABLE_DEFINITION, 0, false},
      {SQL_ODBC_INTERFACE_CONFORMANCE, "SQL_ODBC_INTERFACE_CONFORMANCE", 0, SQL_OIC_CORE, true},
      {SQL_SQL_CONFORMANCE, "SQL_SQL_CONFORMANCE", 0, SQL_SC_SQL92_ENTRY, true},
      {SQL_ASYNC_MODE, "SQL_ASYNC_MODE", 0, SQL_AM_NONE, true},
      {SQL_STRING_FUNCTIONS, "SQL_STRING_FUNCTIONS", SQL_FN_STR_CONCAT | SQL_FN_STR_LCASE | SQL_FN_STR_UCASE | SQL_FN_STR_SUBSTRING | SQL_FN_STR_LENGTH | SQL_FN_STR_LTRIM | SQL_FN_STR_RTRIM | SQL_FN_STR_REPLACE, 0, false},
      {SQL_NUMERIC_FUNCTIONS, "SQL_NUMERIC_FUNCTIONS", SQL_FN_NUM_ABS | SQL_FN_NUM_MOD | SQL_FN_NUM_ROUND | SQL_FN_NUM_FLOOR | SQL_FN_NUM_CEILING | SQL_FN_NUM_SQRT | SQL_FN_NUM_POWER, 0, false},
      {SQL_TIMEDATE_FUNCTIONS, "SQL_TIMEDATE_FUNCTIONS", SQL_FN_TD_CURDATE | SQL_FN_TD_NOW | SQL_FN_TD_YEAR | SQL_FN_TD_MONTH | SQL_FN_TD_EXTRACT | SQL_FN_TD_TIMESTAMPADD | SQL_FN_TD_TIMESTAMPDIFF, 0, false},
      {SQL_SYSTEM_FUNCTIONS, "SQL_SYSTEM_FUNCTIONS", SQL_FN_SYS_DBNAME | SQL_FN_SYS_USERNAME | SQL_FN_SYS_IFNULL, 0, false},
      {SQL_CONVERT_FUNCTIONS, "SQL_CONVERT_FUNCTIONS", SQL_FN_CVT_CAST | SQL_FN_CVT_CONVERT, 0, false},
      {SQL_AGGREGATE_FUNCTIONS, "SQL_AGGREGATE_FUNCTIONS", SQL_AF_COUNT | SQL_AF_SUM | SQL_AF_MIN | SQL_AF_MAX | SQL_AF_AVG | SQL_AF_DISTINCT, 0, false},
      {SQL_TIMEDATE_ADD_INTERVALS, "SQL_TIMEDATE_ADD_INTERVALS", SQL_FN_TSI_DAY | SQL_FN_TSI_SECOND | SQL_FN_TSI_YEAR, 0, false},
      {SQL_TIMEDATE_DIFF_INTERVALS, "SQL_TIMEDATE_DIFF_INTERVALS", SQL_FN_TSI_DAY | SQL_FN_TSI_FRAC_SECOND, 0, false},
      {SQL_SQL92_PREDICATES, "SQL_SQL92_PREDICATES", SQL_SP_LIKE | SQL_SP_IN | SQL_SP_BETWEEN | SQL_SP_EXISTS | SQL_SP_ISNULL, 0, false},
      {SQL_SQL92_VALUE_EXPRESSIONS, "SQL_SQL92_VALUE_EXPRESSIONS", SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF, 0, false},
      {SQL_OJ_CAPABILITIES, "SQL_OJ_CAPABILITIES", SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NESTED, 0, false},
      {SQL_DEFAULT_TXN_ISOLATION, "SQL_DEFAULT_TXN_ISOLATION", 0, SQL_TXN_READ_UNCOMMITTED, true},
      {SQL_TXN_ISOLATION_OPTION, "SQL_TXN_ISOLATION_OPTION", SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED | SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE, 0, false},
      {SQL_PARAM_ARRAY_SELECTS, "SQL_PARAM_ARRAY_SELECTS", 0, SQL_PAS_NO_SELECT, true},
      {SQL_PARAM_ARRAY_ROW_COUNTS, "SQL_PARAM_ARRAY_ROW_COUNTS", 0, SQL_PARC_NO_BATCH, true},
      {SQL_DATETIME_LITERALS, "SQL_DATETIME_LITERALS", SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP, 0, false},
      {SQL_SUBQUERIES, "SQL_SUBQUERIES", SQL_SQ_IN | SQL_SQ_EXISTS | SQL_SQ_CORRELATED_SUBQUERIES, 0, false},
      {SQL_UNION, "SQL_UNION", SQL_U_UNION | SQL_U_UNION_ALL, 0, false},
      {SQL_BOOKMARK_PERSISTENCE, "SQL_BOOKMARK_PERSISTENCE", 0, 0, true},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, "SQL_SQL92_RELATIONAL_JOIN_OPERATORS", SQL_SRJO_INNER_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_CROSS_JOIN, 0, false},
      {SQL_CONVERT_VARCHAR, "SQL_CONVERT_VARCHAR", SQL_CVT_VARCHAR | SQL_CVT_WVARCHAR | SQL_CVT_INTEGER, 0, false},
      {SQL_CONVERT_INTEGER, "SQL_CONVERT_INTEGER", SQL_CVT_VARCHAR | SQL_CVT_BIGINT | SQL_CVT_DOUBLE, 0, false},
      {SQL_INFO_SCHEMA_VIEWS, "SQL_INFO_SCHEMA_VIEWS", SQL_ISV_TABLES | SQL_ISV_COLUMNS, 0, false},
      {SQL_CREATE_TABLE, "SQL_CREATE_TABLE", SQL_CT_CREATE_TABLE, 0, false},
      {SQL_DROP_TABLE, "SQL_DROP_TABLE", SQL_DT_DROP_TABLE, 0, false},
      {SQL_INSERT_STATEMENT, "SQL_INSERT_STATEMENT", SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED, 0, false},
      {SQL_DDL_INDEX, "SQL_DDL_INDEX", 0, 0, true},
      {SQL_BATCH_SUPPORT, "SQL_BATCH_SUPPORT", 0, 0, true},
      {SQL_ASYNC_DBC_FUNCTIONS, "SQL_ASYNC_DBC_FUNCTIONS", 0, 0, true},
      {SQL_DRIVER_AWARE_POOLING_SUPPORTED, "SQL_DRIVER_AWARE_POOLING_SUPPORTED", 0, 0, true},
  };
  for (const UIntExpectation& e : uints) {
    SQLRETURN rc = SQL_ERROR;
    const SQLUINTEGER v = getInfoUInt(s.h(), e.type, &rc);
    testfw::State::get().checks++;
    if (!SQL_SUCCEEDED(rc)) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") failed: " + diagText(SQL_HANDLE_DBC, s.h()), false);
    } else if (e.isExact ? v != e.exact : (v & e.mustHave) != e.mustHave) {
      testfw::fail(__FILE__, __LINE__, std::string("SQLGetInfoW(") + e.name + ") = " + std::to_string(v) +
                                           (e.isExact ? ", expected " + std::to_string(e.exact) : ", missing bits " + std::to_string(e.mustHave & ~v)),
                   false);
    }
  }
}

TEST_CASE("metadata/getinfo_buffer_too_small_01004") {
  Session s;
  SQLWCHAR buf[2] = {0, 0};
  SQLSMALLINT len = 0;
  // Two units of room: one character + terminator for the five-character "Trino".
  const SQLRETURN rc = Odbc.SQLGetInfoW(s.h(), SQL_DBMS_NAME, buf, static_cast<SQLSMALLINT>(sizeof(buf)), &len);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_DBC, s.h(), "01004");
  CHECK_EQ(static_cast<int>(len), static_cast<int>(5 * sizeof(SQLWCHAR)));  // full length in bytes
  CHECK_EQ(fromWide(buf, 1), std::string("T"));
  CHECK(buf[1] == 0);
  // A null buffer just reports the length.
  SQLSMALLINT len2 = 0;
  ODBC_CHECK(Odbc.SQLGetInfoW(s.h(), SQL_DBMS_NAME, nullptr, 0, &len2), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(static_cast<int>(len2), static_cast<int>(5 * sizeof(SQLWCHAR)));
  // ANSI variant: bytes.
  SQLCHAR abuf[3] = {0};
  SQLSMALLINT alen = 0;
  const SQLRETURN rc3 = Odbc.SQLGetInfo(s.h(), SQL_DBMS_NAME, abuf, 3, &alen);
  CHECK(rc3 == SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(static_cast<int>(alen), 5);
  CHECK_EQ(std::string(reinterpret_cast<char*>(abuf)), std::string("Tr"));
}

TEST_CASE("metadata/getinfo_unknown_type_hy096") {
  Session s;
  SQLUINTEGER v = 0;
  SQLSMALLINT len = 0;
  ODBC_CHECK_ERROR(Odbc.SQLGetInfoW(s.h(), 60000, &v, sizeof(v), &len), SQL_HANDLE_DBC, s.h(), "HY096");
}

TEST_CASE("metadata/getfunctions") {
  Session s;
  SQLUSMALLINT bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {0};
  ODBC_REQUIRE(Odbc.SQLGetFunctions(s.h(), SQL_API_ODBC3_ALL_FUNCTIONS, bitmap), SQL_HANDLE_DBC, s.h());
  const SQLUSMALLINT expected[] = {
      SQL_API_SQLALLOCHANDLE, SQL_API_SQLFREEHANDLE, SQL_API_SQLDRIVERCONNECT, SQL_API_SQLCONNECT, SQL_API_SQLDISCONNECT,
      SQL_API_SQLGETINFO, SQL_API_SQLGETFUNCTIONS, SQL_API_SQLGETTYPEINFO, SQL_API_SQLPREPARE, SQL_API_SQLEXECUTE,
      SQL_API_SQLEXECDIRECT, SQL_API_SQLBINDPARAMETER, SQL_API_SQLPARAMDATA, SQL_API_SQLPUTDATA, SQL_API_SQLBINDCOL,
      SQL_API_SQLFETCH, SQL_API_SQLFETCHSCROLL, SQL_API_SQLGETDATA, SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLDESCRIBECOL,
      SQL_API_SQLCOLATTRIBUTE, SQL_API_SQLTABLES, SQL_API_SQLCOLUMNS, SQL_API_SQLPRIMARYKEYS, SQL_API_SQLFOREIGNKEYS,
      SQL_API_SQLSTATISTICS, SQL_API_SQLSPECIALCOLUMNS, SQL_API_SQLPROCEDURES, SQL_API_SQLPROCEDURECOLUMNS,
      SQL_API_SQLTABLEPRIVILEGES, SQL_API_SQLCOLUMNPRIVILEGES, SQL_API_SQLGETDIAGREC, SQL_API_SQLGETDIAGFIELD,
      SQL_API_SQLSETENVATTR, SQL_API_SQLGETENVATTR, SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR,
      SQL_API_SQLSETSTMTATTR, SQL_API_SQLGETSTMTATTR, SQL_API_SQLENDTRAN, SQL_API_SQLNATIVESQL, SQL_API_SQLNUMPARAMS,
      SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLROWCOUNT, SQL_API_SQLMORERESULTS, SQL_API_SQLCLOSECURSOR, SQL_API_SQLCANCEL,
      SQL_API_SQLCOPYDESC, SQL_API_SQLGETDESCFIELD, SQL_API_SQLSETDESCFIELD, SQL_API_SQLGETDESCREC, SQL_API_SQLSETDESCREC,
      SQL_API_SQLSETPOS, SQL_API_SQLBULKOPERATIONS, SQL_API_SQLBROWSECONNECT, SQL_API_SQLSETCURSORNAME,
      SQL_API_SQLGETCURSORNAME, SQL_API_SQLFREESTMT, SQL_API_SQLEXTENDEDFETCH};
  for (SQLUSMALLINT id : expected) {
    testfw::State::get().checks++;
    if (!SQL_FUNC_EXISTS(bitmap, id)) {
      testfw::fail(__FILE__, __LINE__, "SQL_API_ODBC3_ALL_FUNCTIONS lacks function id " + std::to_string(id), false);
    }
  }
  CHECK(SQL_FUNC_EXISTS(bitmap, SQL_API_SQLCANCELHANDLE));
  // ODBC 2 style array.
  SQLUSMALLINT all[100] = {0};
  ODBC_REQUIRE(Odbc.SQLGetFunctions(s.h(), SQL_API_ALL_FUNCTIONS, all), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(all[SQL_API_SQLEXECDIRECT], static_cast<SQLUSMALLINT>(SQL_TRUE));
  CHECK_EQ(all[SQL_API_SQLTABLES], static_cast<SQLUSMALLINT>(SQL_TRUE));
  CHECK_EQ(all[SQL_API_SQLBINDCOL], static_cast<SQLUSMALLINT>(SQL_TRUE));
  // Single ids.
  SQLUSMALLINT one = 0;
  ODBC_CHECK(Odbc.SQLGetFunctions(s.h(), SQL_API_SQLBINDPARAMETER, &one), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(one, static_cast<SQLUSMALLINT>(SQL_TRUE));
  ODBC_CHECK(Odbc.SQLGetFunctions(s.h(), SQL_API_SQLFETCHSCROLL, &one), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(one, static_cast<SQLUSMALLINT>(SQL_TRUE));
}

TEST_CASE("metadata/gettypeinfo_all_types") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(stmt.h, SQL_ALL_TYPES), SQL_HANDLE_STMT, stmt.h);
  const std::vector<std::string> names = columnNames(stmt.h);
  const char* expected[] = {"TYPE_NAME", "DATA_TYPE", "COLUMN_SIZE", "LITERAL_PREFIX", "LITERAL_SUFFIX", "CREATE_PARAMS",
                            "NULLABLE", "CASE_SENSITIVE", "SEARCHABLE", "UNSIGNED_ATTRIBUTE", "FIXED_PREC_SCALE",
                            "AUTO_UNIQUE_VALUE", "LOCAL_TYPE_NAME", "MINIMUM_SCALE", "MAXIMUM_SCALE", "SQL_DATA_TYPE",
                            "SQL_DATETIME_SUB", "NUM_PREC_RADIX", "INTERVAL_PRECISION"};
  REQUIRE_EQ(names.size(), static_cast<size_t>(19));
  for (size_t i = 0; i < 19; ++i) CHECK_EQ(names[i], std::string(expected[i]));
  CHECK_EQ(describeCol(stmt.h, 2).type, static_cast<SQLSMALLINT>(SQL_SMALLINT));
  CHECK_EQ(describeCol(stmt.h, 3).type, static_cast<SQLSMALLINT>(SQL_INTEGER));

  const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
  CHECK(rows.size() >= 12);
  std::set<std::string> typeNames;
  int64_t previous = -100000;
  bool ordered = true;
  bool sawVarchar = false, sawDecimal = false;
  for (const std::vector<std::string>& r : rows) {
    const int64_t dataType = std::atoll(r[1].c_str());
    if (dataType < previous) ordered = false;
    previous = dataType;
    typeNames.insert(r[0]);
    CHECK_EQ(r[6], std::string("1"));  // NULLABLE = SQL_NULLABLE
    CHECK_EQ(r[15].empty() ? "x" : "ok", std::string("ok"));  // SQL_DATA_TYPE present
    if (r[0] == "varchar" && (dataType == SQL_WVARCHAR || dataType == SQL_VARCHAR)) {
      sawVarchar = true;
      CHECK_EQ(r[3], std::string("'"));
      CHECK_EQ(r[4], std::string("'"));
      CHECK_EQ(r[5], std::string("length"));
      CHECK_EQ(r[7], std::string("1"));   // CASE_SENSITIVE
      CHECK_EQ(r[8], std::to_string(SQL_SEARCHABLE));
    }
    if (r[0] == "decimal") {
      sawDecimal = true;
      CHECK_EQ(dataType, static_cast<int64_t>(SQL_DECIMAL));
      CHECK_EQ(r[13], std::string("0"));
      CHECK_EQ(r[14], std::string("38"));
      CHECK_EQ(r[5], std::string("precision,scale"));
      CHECK_EQ(r[17], std::string("10"));
    }
    if (dataType == SQL_TYPE_TIMESTAMP) {
      CHECK_EQ(r[15], std::to_string(SQL_DATETIME));
      CHECK_EQ(r[16], std::to_string(SQL_CODE_TIMESTAMP));
      CHECK_EQ(r[14], std::string("9"));
    }
    if (dataType == SQL_BIGINT) {
      CHECK_EQ(r[2], std::string("19"));
      CHECK_EQ(r[9], std::string("0"));  // signed
      CHECK_EQ(r[3], std::string("<NULL>"));
    }
  }
  CHECK(ordered);
  CHECK(sawVarchar);
  CHECK(sawDecimal);
  for (const char* n : {"boolean", "bigint", "integer", "smallint", "tinyint", "double", "real", "date", "time", "timestamp", "varbinary", "char"}) {
    testfw::State::get().checks++;
    if (!typeNames.count(n)) testfw::fail(__FILE__, __LINE__, std::string("SQLGetTypeInfo lacks type ") + n, false);
  }
}

TEST_CASE("metadata/gettypeinfo_single_and_legacy_types") {
  Session s;
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(stmt.h, SQL_BIGINT), SQL_HANDLE_STMT, stmt.h);
    const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
    REQUIRE(!rows.empty());
    for (const std::vector<std::string>& r : rows) CHECK_EQ(r[1], std::to_string(SQL_BIGINT));
    CHECK_EQ(rows[0][0], std::string("bigint"));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLGetTypeInfo(stmt.h, SQL_TIMESTAMP), SQL_HANDLE_STMT, stmt.h);  // ODBC 2 id -> SQL_TYPE_TIMESTAMP rows
    const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
    REQUIRE(!rows.empty());
    CHECK_EQ(rows[0][1], std::to_string(SQL_TYPE_TIMESTAMP));
  }
  {
    Stmt stmt(s.dbc);
    ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(stmt.h, SQL_WVARCHAR), SQL_HANDLE_STMT, stmt.h);
    CHECK(countRows(stmt.h) >= 1);
  }
  {
    Stmt stmt(s.dbc);
    ODBC_CHECK_ERROR(Odbc.SQLGetTypeInfoW(stmt.h, 9999), SQL_HANDLE_STMT, stmt.h, "HY004");
  }
  {
    // Interval rows only appear when IntervalMode=interval.
    Session iv("IntervalMode=interval");
    Stmt stmt(iv.dbc);
    ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(stmt.h, SQL_INTERVAL_DAY_TO_SECOND), SQL_HANDLE_STMT, stmt.h);
    const std::vector<std::vector<std::string>> rows = fetchAll(stmt.h);
    REQUIRE_EQ(rows.size(), static_cast<size_t>(1));
    CHECK_EQ(rows[0][15], std::to_string(SQL_INTERVAL));
    CHECK_EQ(rows[0][16], std::to_string(SQL_CODE_DAY_TO_SECOND));
    Stmt none(s.dbc);
    ODBC_REQUIRE(Odbc.SQLGetTypeInfoW(none.h, SQL_INTERVAL_DAY_TO_SECOND), SQL_HANDLE_STMT, none.h);
    CHECK_EQ(countRows(none.h), static_cast<int64_t>(0));
  }
}

TEST_CASE("metadata/env_attr_roundtrip") {
  Env env;
  SQLINTEGER v = 0;
  SQLINTEGER len = 0;
  ODBC_CHECK(Odbc.SQLGetEnvAttr(env.h, SQL_ATTR_ODBC_VERSION, &v, sizeof(v), &len), SQL_HANDLE_ENV, env.h);
  CHECK_EQ(v, static_cast<SQLINTEGER>(SQL_OV_ODBC3_80));
  ODBC_CHECK(Odbc.SQLGetEnvAttr(env.h, SQL_ATTR_OUTPUT_NTS, &v, sizeof(v), &len), SQL_HANDLE_ENV, env.h);
  CHECK_EQ(v, static_cast<SQLINTEGER>(SQL_TRUE));
  ODBC_CHECK(Odbc.SQLSetEnvAttr(env.h, SQL_ATTR_OUTPUT_NTS, attrVal(SQL_TRUE), 0), SQL_HANDLE_ENV, env.h);
  CHECK(Odbc.SQLSetEnvAttr(env.h, SQL_ATTR_OUTPUT_NTS, attrVal(SQL_FALSE), 0) != SQL_SUCCESS);
  ODBC_CHECK(Odbc.SQLGetEnvAttr(env.h, SQL_ATTR_CONNECTION_POOLING, &v, sizeof(v), &len), SQL_HANDLE_ENV, env.h);
  Env v3(static_cast<SQLINTEGER>(SQL_OV_ODBC3));
  ODBC_CHECK(Odbc.SQLGetEnvAttr(v3.h, SQL_ATTR_ODBC_VERSION, &v, sizeof(v), &len), SQL_HANDLE_ENV, v3.h);
  CHECK_EQ(v, static_cast<SQLINTEGER>(SQL_OV_ODBC3));
}

TEST_CASE("metadata/connect_attr_roundtrip") {
  Session s;
  SQLRETURN rc = SQL_SUCCESS;
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_AUTOCOMMIT, &rc), static_cast<SQLULEN>(SQL_AUTOCOMMIT_ON));
  ODBC_CHECK(rc, SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_ACCESS_MODE, &rc), static_cast<SQLULEN>(SQL_MODE_READ_WRITE));
  ODBC_CHECK(rc, SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_CONNECTION_TIMEOUT, attrVal(42), 0), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_CONNECTION_TIMEOUT), static_cast<SQLULEN>(42));
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_AUTO_IPD, &rc), static_cast<SQLULEN>(SQL_TRUE));
  ODBC_CHECK(rc, SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_METADATA_ID, attrVal(SQL_TRUE), 0), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_METADATA_ID), static_cast<SQLULEN>(SQL_TRUE));
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_METADATA_ID, attrVal(SQL_FALSE), 0), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_TXN_ISOLATION, attrVal(SQL_TXN_SERIALIZABLE), 0), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_TXN_ISOLATION), static_cast<SQLULEN>(SQL_TXN_SERIALIZABLE));
  // Autocommit off is an optional feature without EnableTransactions.
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_AUTOCOMMIT, attrVal(SQL_AUTOCOMMIT_OFF), 0), SQL_HANDLE_DBC, s.h(), "HYC00");
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_AUTOCOMMIT), static_cast<SQLULEN>(SQL_AUTOCOMMIT_ON));
  // Unknown attribute.
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectAttrW(s.h(), 99999, attrVal(1), 0), SQL_HANDLE_DBC, s.h(), "HY092");
  // String attribute through the wide getter (byte lengths).
  if (ctx().mock) {
    std::vector<SQLWCHAR> buf(256);
    SQLINTEGER lenBytes = 0;
    ODBC_CHECK(Odbc.SQLGetConnectAttrW(s.h(), SQL_ATTR_CURRENT_CATALOG, buf.data(),
                                       static_cast<SQLINTEGER>(buf.size() * sizeof(SQLWCHAR)), &lenBytes),
               SQL_HANDLE_DBC, s.h());
    CHECK_EQ(fromWide(buf.data(), lenBytes / static_cast<SQLINTEGER>(sizeof(SQLWCHAR))), std::string("mock"));
    // Narrow getter too.
    char abuf[64] = {0};
    SQLINTEGER alen = 0;
    ODBC_CHECK(Odbc.SQLGetConnectAttr(s.h(), SQL_ATTR_CURRENT_CATALOG, abuf, sizeof(abuf), &alen), SQL_HANDLE_DBC, s.h());
    CHECK_EQ(std::string(abuf), std::string("mock"));
  }
  // ODBC 2 option wrappers.
  SQLULEN opt = 0;
  ODBC_CHECK(Odbc.SQLGetConnectOption(s.h(), SQL_AUTOCOMMIT, &opt), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(opt, static_cast<SQLULEN>(SQL_AUTOCOMMIT_ON));
  ODBC_CHECK(Odbc.SQLSetConnectOption(s.h(), SQL_LOGIN_TIMEOUT, 7), SQL_HANDLE_DBC, s.h());
}

TEST_CASE("metadata/stmt_attr_roundtrip") {
  Session s;
  Stmt stmt(s.dbc);
  SQLRETURN rc = SQL_SUCCESS;
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, &rc), static_cast<SQLULEN>(1));
  ODBC_CHECK(rc, SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(100), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), static_cast<SQLULEN>(100));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_ROWS, attrVal(10), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_MAX_ROWS), static_cast<SQLULEN>(10));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_QUERY_TIMEOUT, attrVal(30), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_QUERY_TIMEOUT), static_cast<SQLULEN>(30));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_MAX_LENGTH, attrVal(4096), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_MAX_LENGTH), static_cast<SQLULEN>(4096));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_CURSOR_TYPE), static_cast<SQLULEN>(SQL_CURSOR_FORWARD_ONLY));
  // Requests for other cursor types are downgraded with 01S02.
  rc = Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_CURSOR_TYPE, attrVal(SQL_CURSOR_STATIC), 0);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_STMT, stmt.h, "01S02");
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_CURSOR_TYPE), static_cast<SQLULEN>(SQL_CURSOR_FORWARD_ONLY));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_CONCURRENCY), static_cast<SQLULEN>(SQL_CONCUR_READ_ONLY));
  rc = Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_CONCURRENCY, attrVal(SQL_CONCUR_LOCK), 0);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_CONCURRENCY), static_cast<SQLULEN>(SQL_CONCUR_READ_ONLY));
  rc = Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_CURSOR_SCROLLABLE, attrVal(SQL_SCROLLABLE), 0);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_NOSCAN, attrVal(SQL_NOSCAN_ON), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_NOSCAN), static_cast<SQLULEN>(SQL_NOSCAN_ON));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_NOSCAN, attrVal(SQL_NOSCAN_OFF), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_METADATA_ID, attrVal(SQL_TRUE), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_METADATA_ID), static_cast<SQLULEN>(SQL_TRUE));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_METADATA_ID, attrVal(SQL_FALSE), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_PARAMSET_SIZE, attrVal(3), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_PARAMSET_SIZE), static_cast<SQLULEN>(3));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_TYPE, attrVal(64), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_BIND_TYPE), static_cast<SQLULEN>(64));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_BIND_TYPE, attrVal(SQL_BIND_BY_COLUMN), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_RETRIEVE_DATA, attrVal(SQL_RD_ON), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_KEYSET_SIZE, attrVal(0), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_USE_BOOKMARKS, attrVal(SQL_UB_OFF), 0), SQL_HANDLE_STMT, stmt.h);
  CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_USE_BOOKMARKS, attrVal(SQL_UB_VARIABLE), 0) != SQL_SUCCESS);
  rc = Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ASYNC_ENABLE, attrVal(SQL_ASYNC_ENABLE_ON), 0);
  CHECK(rc == SQL_SUCCESS_WITH_INFO || rc == SQL_ERROR);
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, 99999, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h, "HY092");
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_FETCH_BOOKMARK_PTR, attrVal(1), 0), SQL_HANDLE_STMT, stmt.h, "HYC00");
  // ODBC 2 option wrappers. SQL_ROWSET_SIZE is the SQLExtendedFetch rowset and is kept apart from
  // SQL_ATTR_ROW_ARRAY_SIZE (the SQLFetch / SQLFetchScroll rowset).
  const SQLULEN arraySizeBefore = getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE);
  ODBC_CHECK(Odbc.SQLSetStmtOption(stmt.h, SQL_ROWSET_SIZE, 7), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ROWSET_SIZE), static_cast<SQLULEN>(7));
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), arraySizeBefore);
  SQLULEN opt = 0;
  ODBC_CHECK(Odbc.SQLGetStmtOption(stmt.h, SQL_MAX_ROWS, &opt), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(opt, static_cast<SQLULEN>(10));
}

TEST_CASE("metadata/descriptor_handles") {
  Session s;
  Stmt stmt(s.dbc);
  SQLHDESC ard = nullptr, apd = nullptr, ird = nullptr, ipd = nullptr;
  SQLINTEGER len = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &ard, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_PARAM_DESC, &apd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_ROW_DESC, &ird, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_PARAM_DESC, &ipd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK(ard && apd && ird && ipd);
  CHECK(ard != apd && ard != ird && ard != ipd && apd != ipd && ird != ipd);
  SQLSMALLINT allocType = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ard, 0, SQL_DESC_ALLOC_TYPE, &allocType, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ard);
  CHECK_EQ(allocType, static_cast<SQLSMALLINT>(SQL_DESC_ALLOC_AUTO));
  SQLULEN arraySize = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ard, 0, SQL_DESC_ARRAY_SIZE, &arraySize, SQL_IS_UINTEGER, &len), SQL_HANDLE_DESC, ard);
  CHECK_EQ(arraySize, static_cast<SQLULEN>(1));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE, attrVal(5), 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ard, 0, SQL_DESC_ARRAY_SIZE, &arraySize, SQL_IS_UINTEGER, &len), SQL_HANDLE_DESC, ard);
  CHECK_EQ(arraySize, static_cast<SQLULEN>(5));
  // Setting the header through the descriptor is visible through the statement attribute.
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ard, 0, SQL_DESC_ARRAY_SIZE, attrVal(9), 0), SQL_HANDLE_DESC, ard);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), static_cast<SQLULEN>(9));
  // IRD is read-only; IRD/IPD cannot be set as statement descriptors.
  ODBC_CHECK_ERROR(Odbc.SQLSetDescFieldW(ird, 0, SQL_DESC_COUNT, attrVal(1), 0), SQL_HANDLE_DESC, ird, "HY016");
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_IMP_ROW_DESC, ird, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h, "HY017");

  SQLHDESC explicitDesc = nullptr;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_DESC, s.h(), &explicitDesc), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLGetDescFieldW(explicitDesc, 0, SQL_DESC_ALLOC_TYPE, &allocType, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, explicitDesc);
  CHECK_EQ(allocType, static_cast<SQLSMALLINT>(SQL_DESC_ALLOC_USER));
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, explicitDesc, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h);
  SQLHDESC current = nullptr;
  ODBC_CHECK(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &current, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK(current == explicitDesc);
  // Freeing the explicit descriptor reverts the statement to its implicit ARD.
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DESC, explicitDesc), SQL_HANDLE_DESC, explicitDesc);
  ODBC_CHECK(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &current, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK(current == ard);
  // SQL_NULL_HDESC also reverts.
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_DESC, s.h(), &explicitDesc), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_APP_PARAM_DESC, explicitDesc, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_APP_PARAM_DESC, SQL_NULL_HDESC, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_PARAM_DESC, &current, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK(current == apd);
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DESC, explicitDesc), SQL_HANDLE_DESC, explicitDesc);
  // A descriptor of another connection is rejected.
  Session other;
  SQLHDESC foreign = nullptr;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_DESC, other.h(), &foreign), SQL_HANDLE_DBC, other.h());
  ODBC_CHECK_ERROR(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, foreign, SQL_IS_POINTER), SQL_HANDLE_STMT, stmt.h, "HY024");
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DESC, foreign), SQL_HANDLE_DESC, foreign);
}

TEST_CASE("metadata/descriptor_binding_path") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  SQLHDESC ard = nullptr, ird = nullptr;
  SQLINTEGER len = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_APP_ROW_DESC, &ard, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_ROW_DESC, &ird, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  // IRD describes the result.
  SQLSMALLINT count = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ird, 0, SQL_DESC_COUNT, &count, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ird);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(1));
  std::vector<SQLWCHAR> name(128);
  SQLSMALLINT nameLen = 0, type = 0, sub = 0, precision = 0, scale = 0, nullable = 0;
  SQLLEN length = 0;
  ODBC_CHECK(Odbc.SQLGetDescRecW(ird, 1, name.data(), 128, &nameLen, &type, &sub, &length, &precision, &scale, &nullable), SQL_HANDLE_DESC, ird);
  CHECK_EQ(fromWide(name.data(), nameLen), std::string("_col0"));
  CHECK_EQ(type, static_cast<SQLSMALLINT>(SQL_INTEGER));
  // Bind through the ARD: data pointer last, as the spec prescribes.
  SQLINTEGER value = 0;
  SQLLEN ind = 0;
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ard, 1, SQL_DESC_CONCISE_TYPE, attrVal(SQL_C_SLONG), 0), SQL_HANDLE_DESC, ard);
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ard, 1, SQL_DESC_OCTET_LENGTH_PTR, &ind, SQL_IS_POINTER), SQL_HANDLE_DESC, ard);
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ard, 1, SQL_DESC_INDICATOR_PTR, &ind, SQL_IS_POINTER), SQL_HANDLE_DESC, ard);
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ard, 1, SQL_DESC_DATA_PTR, &value, SQL_IS_POINTER), SQL_HANDLE_DESC, ard);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ard, 0, SQL_DESC_COUNT, &count, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ard);
  CHECK_EQ(count, static_cast<SQLSMALLINT>(1));
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(value, static_cast<SQLINTEGER>(1));
  CHECK_EQ(ind, static_cast<SQLLEN>(sizeof(SQLINTEGER)));
  // SQLBindCol writes the same record.
  SQLBIGINT big = 0;
  ODBC_CHECK(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SBIGINT, &big, sizeof(big), &ind), SQL_HANDLE_STMT, stmt.h);
  SQLSMALLINT concise = 0;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ard, 1, SQL_DESC_CONCISE_TYPE, &concise, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, ard);
  CHECK_EQ(concise, static_cast<SQLSMALLINT>(SQL_C_SBIGINT));
  // Copying descriptors: ARD -> explicit works, anything -> IRD is HY016.
  SQLHDESC copy = nullptr;
  ODBC_REQUIRE(Odbc.SQLAllocHandle(SQL_HANDLE_DESC, s.h(), &copy), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLCopyDesc(ard, copy), SQL_HANDLE_DESC, copy);
  ODBC_CHECK(Odbc.SQLGetDescFieldW(copy, 1, SQL_DESC_CONCISE_TYPE, &concise, SQL_IS_SMALLINT, &len), SQL_HANDLE_DESC, copy);
  CHECK_EQ(concise, static_cast<SQLSMALLINT>(SQL_C_SBIGINT));
  ODBC_CHECK_ERROR(Odbc.SQLCopyDesc(copy, ird), SQL_HANDLE_DESC, ird, "HY016");
  ODBC_CHECK(Odbc.SQLFreeHandle(SQL_HANDLE_DESC, copy), SQL_HANDLE_DESC, copy);
  // Record 0 of the ARD holds no data; unknown field id.
  ODBC_CHECK_ERROR(Odbc.SQLSetDescFieldW(ard, 1, 32000, attrVal(1), 0), SQL_HANDLE_DESC, ard, "HY091");
}

TEST_CASE("metadata/native_sql") {
  Session s;
  const std::string sql = "SELECT {fn UCASE(name)}, {fn IFNULL(x, 0)} FROM t WHERE d = {d '2024-01-02'} AND ts > {ts '2024-01-02 03:04:05'}";
  WStr in(sql);
  std::vector<SQLWCHAR> out(1024);
  SQLINTEGER outLen = 0;
  ODBC_REQUIRE(Odbc.SQLNativeSqlW(s.h(), in.ptr(), in.ilen(), out.data(), static_cast<SQLINTEGER>(out.size()), &outLen), SQL_HANDLE_DBC, s.h());
  const std::string native = fromWide(out.data(), outLen);
  CHECK(native.find("upper(name)") != std::string::npos);
  CHECK(native.find("coalesce(x, 0)") != std::string::npos);
  CHECK(native.find("DATE '2024-01-02'") != std::string::npos);
  CHECK(native.find("TIMESTAMP '2024-01-02 03:04:05'") != std::string::npos);
  CHECK(native.find("{fn") == std::string::npos);
  // Truncation reports the full length.
  SQLWCHAR small[8];
  SQLINTEGER fullLen = 0;
  const SQLRETURN rc = Odbc.SQLNativeSqlW(s.h(), in.ptr(), in.ilen(), small, 8, &fullLen);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  ODBC_CHECK_STATE(SQL_HANDLE_DBC, s.h(), "01004");
  CHECK_EQ(fullLen, outLen);
  // ANSI variant.
  std::vector<SQLCHAR> aout(1024);
  SQLINTEGER alen = 0;
  ODBC_CHECK(Odbc.SQLNativeSql(s.h(), narrow(sql), static_cast<SQLINTEGER>(sql.size()), aout.data(), 1024, &alen), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(std::string(reinterpret_cast<char*>(aout.data()), static_cast<size_t>(alen)), native);
  // {? = call} is not supported.
  WStr call("{? = call p(1)}");
  ODBC_CHECK_ERROR(Odbc.SQLNativeSqlW(s.h(), call.ptr(), call.ilen(), out.data(), 1024, &outLen), SQL_HANDLE_DBC, s.h(), "HYC00");
}

TEST_CASE("metadata/cursor_name") {
  Session s;
  Stmt stmt(s.dbc);
  std::vector<SQLWCHAR> name(128);
  SQLSMALLINT len = 0;
  ODBC_REQUIRE(Odbc.SQLGetCursorNameW(stmt.h, name.data(), 128, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK(fromWide(name.data(), len).rfind("SQL_CUR", 0) == 0);
  WStr mine("my_cursor");
  ODBC_CHECK(Odbc.SQLSetCursorNameW(stmt.h, mine.ptr(), mine.len()), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLGetCursorNameW(stmt.h, name.data(), 128, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fromWide(name.data(), len), std::string("my_cursor"));
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT 1"), SQL_HANDLE_STMT, stmt.h);
  WStr other("other");
  ODBC_CHECK_ERROR(Odbc.SQLSetCursorNameW(stmt.h, other.ptr(), other.len()), SQL_HANDLE_STMT, stmt.h, "24000");
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);
  WStr empty("");
  ODBC_CHECK_ERROR(Odbc.SQLSetCursorNameW(stmt.h, empty.ptr(), 0), SQL_HANDLE_STMT, stmt.h, "34000");
  // ANSI getter.
  SQLCHAR aname[64] = {0};
  ODBC_CHECK(Odbc.SQLGetCursorName(stmt.h, aname, 64, &len), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(std::string(reinterpret_cast<char*>(aname)), std::string("my_cursor"));
}

TEST_CASE("metadata/endtran_autocommit_noop") {
  Session s;
  ODBC_CHECK(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_ROLLBACK), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLEndTran(SQL_HANDLE_ENV, s.env.h, SQL_COMMIT), SQL_HANDLE_ENV, s.env.h);
  ODBC_CHECK(Odbc.SQLTransact(s.env.h, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  CHECK(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), 99) == SQL_ERROR);
}

TEST_CASE("metadata/transactions_enabled_reports_dml") {
  HARNESS_MOCK_ONLY();
  Session s("EnableTransactions=1");
  CHECK_EQ(getInfoUSmall(s.h(), SQL_TXN_CAPABLE), static_cast<SQLUSMALLINT>(SQL_TC_DML));
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_AUTOCOMMIT, attrVal(SQL_AUTOCOMMIT_OFF), 0), SQL_HANDLE_DBC, s.h());
  CHECK_EQ(getConnectAttrInt(s.h(), SQL_ATTR_AUTOCOMMIT), static_cast<SQLULEN>(SQL_AUTOCOMMIT_OFF));
  Stmt stmt(s.dbc);
  ODBC_CHECK(execDirect(stmt.h, "INSERT INTO t VALUES (1)"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_COMMIT), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(execDirect(stmt.h, "INSERT INTO t VALUES (2)"), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLEndTran(SQL_HANDLE_DBC, s.h(), SQL_ROLLBACK), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectAttrW(s.h(), SQL_ATTR_AUTOCOMMIT, attrVal(SQL_AUTOCOMMIT_ON), 0), SQL_HANDLE_DBC, s.h());
}

// Review findings [57] [58] [59] [60]: capability bitmasks describe what Trino really accepts.
TEST_CASE("metadata/getinfo_matches_trino_capabilities") {
  Session s;
  // [58] Trino sorts NULLs last whatever the direction.
  CHECK_EQ(getInfoUSmall(s.h(), SQL_NULL_COLLATION), static_cast<SQLUSMALLINT>(SQL_NC_END));
  // [59] Only EXTRACT and POSITION exist natively (no char_length / character_length / octet_length).
  CHECK_EQ(getInfoUInt(s.h(), SQL_SQL92_NUMERIC_VALUE_FUNCTIONS),
           static_cast<SQLUINTEGER>(SQL_SNVF_EXTRACT | SQL_SNVF_POSITION));
  // [60] NATURAL JOIN is rejected by the analyzer and UNION JOIN is not in the grammar.
  const SQLUINTEGER joins = getInfoUInt(s.h(), SQL_SQL92_RELATIONAL_JOIN_OPERATORS);
  CHECK_EQ(joins, static_cast<SQLUINTEGER>(SQL_SRJO_CROSS_JOIN | SQL_SRJO_EXCEPT_JOIN | SQL_SRJO_FULL_OUTER_JOIN |
                                           SQL_SRJO_INNER_JOIN | SQL_SRJO_INTERSECT_JOIN | SQL_SRJO_LEFT_OUTER_JOIN |
                                           SQL_SRJO_RIGHT_OUTER_JOIN));
  CHECK((joins & (SQL_SRJO_NATURAL_JOIN | SQL_SRJO_UNION_JOIN)) == 0);
  // [57] varbinary has no cast to varchar; varchar has no cast to interval.
  const SQLUSMALLINT binaryTypes[] = {SQL_CONVERT_BINARY, SQL_CONVERT_VARBINARY, SQL_CONVERT_LONGVARBINARY};
  for (const SQLUSMALLINT t : binaryTypes) {
    const SQLUINTEGER v = getInfoUInt(s.h(), t);
    CHECK((v & (SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR | SQL_CVT_WCHAR | SQL_CVT_WVARCHAR |
                SQL_CVT_WLONGVARCHAR)) == 0);
    CHECK((v & SQL_CVT_VARBINARY) != 0);
  }
  const SQLUSMALLINT charTypes[] = {SQL_CONVERT_CHAR,  SQL_CONVERT_VARCHAR,  SQL_CONVERT_LONGVARCHAR,
                                    SQL_CONVERT_WCHAR, SQL_CONVERT_WVARCHAR, SQL_CONVERT_WLONGVARCHAR};
  for (const SQLUSMALLINT t : charTypes) {
    const SQLUINTEGER v = getInfoUInt(s.h(), t);
    CHECK((v & (SQL_CVT_INTERVAL_YEAR_MONTH | SQL_CVT_INTERVAL_DAY_TIME)) == 0);
    CHECK((v & SQL_CVT_INTEGER) != 0);
  }
}

// Review finding [24]: with MetadataCaseSensitive=1 catalog arguments are literals, so no search
// pattern escape is advertised (an application would otherwise escape '_' and never match).
TEST_CASE("metadata/search_pattern_escape_follows_literal_mode") {
  {
    Session s;
    CHECK_EQ(getInfoString(s.h(), SQL_SEARCH_PATTERN_ESCAPE), std::string("\\"));
  }
  Session literal("MetadataCaseSensitive=1");
  SQLRETURN rc = SQL_ERROR;
  CHECK_EQ(getInfoString(literal.h(), SQL_SEARCH_PATTERN_ESCAPE, &rc), std::string());
  ODBC_CHECK(rc, SQL_HANDLE_DBC, literal.h());
}

// Review findings [4] [5]: SQL_ROWSET_SIZE (SQLExtendedFetch) and SQL_ATTR_ROW_ARRAY_SIZE (SQLFetch)
// are separate: an ODBC 2 rowset size must never make SQLFetch write past single-row buffers.
TEST_CASE("metadata/rowset_size_does_not_change_sqlfetch_rowset") {
  HARNESS_MOCK_ONLY();
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtOption(stmt.h, SQL_ROWSET_SIZE, 20), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(getStmtAttrInt(stmt.h, SQL_ATTR_ROW_ARRAY_SIZE), static_cast<SQLULEN>(1));
  SQLULEN rowset = 0;
  ODBC_CHECK(Odbc.SQLGetStmtOption(stmt.h, SQL_ROWSET_SIZE, &rowset), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(rowset, static_cast<SQLULEN>(20));

  // SQLExtendedFetch uses the ODBC 2 rowset size.
  SQLINTEGER ids[20];
  SQLLEN inds[20];
  for (int i = 0; i < 20; ++i) ids[i] = -1;
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM big_table /* rows=50 */"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SLONG, ids, 0, inds), SQL_HANDLE_STMT, stmt.h);
  SQLULEN crow = 0;
  SQLUSMALLINT status[20];
  ODBC_REQUIRE(Odbc.SQLExtendedFetch(stmt.h, SQL_FETCH_NEXT, 0, &crow, status), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(crow, static_cast<SQLULEN>(20));
  CHECK_EQ(ids[19], static_cast<SQLINTEGER>(20));
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLCloseCursor(stmt.h), SQL_HANDLE_STMT, stmt.h);

  // SQLFetch on the same handle with scalar buffers fetches exactly one row.
  struct {
    SQLINTEGER value;
    SQLINTEGER guard[19];
  } one;
  struct {
    SQLLEN ind;
    SQLLEN guard[19];
  } oneInd;
  one.value = -1;
  oneInd.ind = 0;
  for (int i = 0; i < 19; ++i) {
    one.guard[i] = 0x5A5A5A5A;
    oneInd.guard[i] = 0x5A5A5A5A;
  }
  SQLULEN fetched = 0;
  ODBC_REQUIRE(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(execDirect(stmt.h, "SELECT * FROM big_table /* rows=50 */"), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLBindCol(stmt.h, 1, SQL_C_SLONG, &one.value, 0, &oneInd.ind), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLFetch(stmt.h), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(fetched, static_cast<SQLULEN>(1));
  CHECK_EQ(one.value, static_cast<SQLINTEGER>(1));
  bool guardsIntact = true;
  for (int i = 0; i < 19; ++i) {
    if (one.guard[i] != 0x5A5A5A5A || oneInd.guard[i] != 0x5A5A5A5A) guardsIntact = false;
  }
  CHECK(guardsIntact);
  ODBC_CHECK(Odbc.SQLSetStmtAttrW(stmt.h, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK(Odbc.SQLFreeStmt(stmt.h, SQL_UNBIND), SQL_HANDLE_STMT, stmt.h);
}

// Review finding [52]: ODBC 2 statement options set with SQLSetConnectOption apply to the statements
// of the connection instead of failing with HY092.
TEST_CASE("metadata/statement_options_at_connection_level") {
  Session s;
  Stmt first(s.dbc);
  Stmt second(s.dbc);
  ODBC_CHECK(Odbc.SQLSetConnectOption(s.h(), SQL_MAX_ROWS, 5), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectOption(s.h(), SQL_QUERY_TIMEOUT, 30), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectOptionW(s.h(), SQL_NOSCAN, SQL_NOSCAN_ON), SQL_HANDLE_DBC, s.h());
  ODBC_CHECK(Odbc.SQLSetConnectOption(s.h(), SQL_ROWSET_SIZE, 4), SQL_HANDLE_DBC, s.h());
  for (SQLHSTMT h : {first.h, second.h}) {
    CHECK_EQ(getStmtAttrInt(h, SQL_ATTR_MAX_ROWS), static_cast<SQLULEN>(5));
    CHECK_EQ(getStmtAttrInt(h, SQL_ATTR_QUERY_TIMEOUT), static_cast<SQLULEN>(30));
    CHECK_EQ(getStmtAttrInt(h, SQL_ATTR_NOSCAN), static_cast<SQLULEN>(SQL_NOSCAN_ON));
    CHECK_EQ(getStmtAttrInt(h, SQL_ROWSET_SIZE), static_cast<SQLULEN>(4));
    CHECK_EQ(getStmtAttrInt(h, SQL_ATTR_ROW_ARRAY_SIZE), static_cast<SQLULEN>(1));
  }
  // A substituted value is reported once on the connection (01S02), not once per statement.
  const SQLRETURN rc = Odbc.SQLSetConnectOption(s.h(), SQL_CURSOR_TYPE, SQL_CURSOR_STATIC);
  CHECK(rc == SQL_SUCCESS_WITH_INFO);
  const std::vector<DiagRec> d = diag(SQL_HANDLE_DBC, s.h());
  CHECK_EQ(d.size(), static_cast<size_t>(1));
  CHECK(hasState(d, "01S02"));
  CHECK_EQ(getStmtAttrInt(first.h, SQL_ATTR_CURSOR_TYPE), static_cast<SQLULEN>(SQL_CURSOR_FORWARD_ONLY));
  // Invalid values still fail.
  ODBC_CHECK_ERROR(Odbc.SQLSetConnectOption(s.h(), SQL_NOSCAN, 9), SQL_HANDLE_DBC, s.h(), "HY024");
}

// Review finding [53]: SQLGetStmtOption writes 32-bit values for the ODBC 2 UDWORD options and
// SQLULEN only for SQL_KEYSET_SIZE / SQL_MAX_LENGTH / SQL_MAX_ROWS / SQL_ROWSET_SIZE.
TEST_CASE("metadata/getstmtoption_value_widths") {
  Session s;
  Stmt stmt(s.dbc);
  ODBC_REQUIRE(Odbc.SQLSetStmtOption(stmt.h, SQL_QUERY_TIMEOUT, 30), SQL_HANDLE_STMT, stmt.h);
  ODBC_REQUIRE(Odbc.SQLSetStmtOption(stmt.h, SQL_MAX_ROWS, 12), SQL_HANDLE_STMT, stmt.h);
  const SQLUSMALLINT narrowOptions[] = {SQL_QUERY_TIMEOUT, SQL_NOSCAN,          SQL_ASYNC_ENABLE, SQL_BIND_TYPE,
                                        SQL_CURSOR_TYPE,   SQL_CONCURRENCY,     SQL_RETRIEVE_DATA, SQL_USE_BOOKMARKS,
                                        SQL_SIMULATE_CURSOR};
  for (const SQLUSMALLINT option : narrowOptions) {
    struct {
      SQLUINTEGER value;
      SQLUINTEGER guard;
    } buf = {0xFFFFFFFFu, 0xA5A5A5A5u};
    ODBC_CHECK(Odbc.SQLGetStmtOption(stmt.h, option, &buf.value), SQL_HANDLE_STMT, stmt.h);
    CHECK_EQ(buf.guard, static_cast<SQLUINTEGER>(0xA5A5A5A5u));
    if (option == SQL_QUERY_TIMEOUT) CHECK_EQ(buf.value, static_cast<SQLUINTEGER>(30));
    if (option == SQL_CURSOR_TYPE) CHECK_EQ(buf.value, static_cast<SQLUINTEGER>(SQL_CURSOR_FORWARD_ONLY));
  }
  SQLULEN wide = 0;
  ODBC_CHECK(Odbc.SQLGetStmtOption(stmt.h, SQL_MAX_ROWS, &wide), SQL_HANDLE_STMT, stmt.h);
  CHECK_EQ(wide, static_cast<SQLULEN>(12));
}

// Review finding [56]: SQL_DESC_DATA_PTR on an IPD runs the record consistency check and is not stored.
TEST_CASE("metadata/ipd_data_ptr_consistency_check") {
  Session s;
  Stmt stmt(s.dbc);
  SQLHDESC ipd = nullptr;
  SQLINTEGER len = 0;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_PARAM_DESC, &ipd, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  SQLINTEGER x = 0;
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd, 1, SQL_DESC_CONCISE_TYPE, attrVal(SQL_INTEGER), SQL_IS_SMALLINT),
               SQL_HANDLE_DESC, ipd);
  ODBC_CHECK(Odbc.SQLSetDescFieldW(ipd, 1, SQL_DESC_DATA_PTR, &x, SQL_IS_POINTER), SQL_HANDLE_DESC, ipd);
  SQLPOINTER stored = &x;
  ODBC_CHECK(Odbc.SQLGetDescFieldW(ipd, 1, SQL_DESC_DATA_PTR, &stored, SQL_IS_POINTER, &len), SQL_HANDLE_DESC, ipd);
  CHECK(stored == nullptr);
  // An inconsistent record fails the check with HY021.
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd, 2, SQL_DESC_CONCISE_TYPE, attrVal(SQL_DECIMAL), SQL_IS_SMALLINT),
               SQL_HANDLE_DESC, ipd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd, 2, SQL_DESC_PRECISION, attrVal(10), SQL_IS_SMALLINT), SQL_HANDLE_DESC, ipd);
  ODBC_REQUIRE(Odbc.SQLSetDescFieldW(ipd, 2, SQL_DESC_SCALE, attrVal(12), SQL_IS_SMALLINT), SQL_HANDLE_DESC, ipd);
  ODBC_CHECK_ERROR(Odbc.SQLSetDescFieldW(ipd, 2, SQL_DESC_DATA_PTR, &x, SQL_IS_POINTER), SQL_HANDLE_DESC, ipd, "HY021");
  // The IRD stays unmodifiable (HY016).
  SQLHDESC ird = nullptr;
  ODBC_REQUIRE(Odbc.SQLGetStmtAttrW(stmt.h, SQL_ATTR_IMP_ROW_DESC, &ird, SQL_IS_POINTER, &len), SQL_HANDLE_STMT, stmt.h);
  ODBC_CHECK_ERROR(Odbc.SQLSetDescFieldW(ird, 1, SQL_DESC_DATA_PTR, &x, SQL_IS_POINTER), SQL_HANDLE_DESC, ird, "HY016");
}

// Review findings [67] [68] [23]: SQLGetDiagRecW rejects a negative BufferLength like the ANSI entry
// point, and SQLErrorW returns the full Unicode message (never through the ANSI code page) with the
// same one-record-per-call sequencing as SQLError.
TEST_CASE("metadata/wide_diagnostics_entry_points") {
  Env env;
  Dbc dbc(env);
  // The invalid value is echoed in the message, which gives a non-ASCII diagnostic without a server.
  const std::string bad = "m\xC3\xBC\xC5\x9Fteri";  // "müşteri"
  // TargetResultSize never appears in the harness connection strings, so this value is the first
  // (winning) occurrence whichever server the suite runs against.
  WStr in(ctx().connStrWith("TargetResultSize=" + bad));
  CHECK(Odbc.SQLDriverConnectW(dbc.h, nullptr, in.ptr(), in.len(), nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT) == SQL_ERROR);

  SQLWCHAR state[6] = {0};
  SQLINTEGER native = 0;
  SQLWCHAR msg[512] = {0};
  SQLSMALLINT msgLen = 0;
  CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_DBC, dbc.h, 1, state, &native, msg, -1, &msgLen) == SQL_ERROR);
  CHECK(Odbc.SQLGetDiagRec(SQL_HANDLE_DBC, dbc.h, 1, nullptr, &native, nullptr, -1, &msgLen) == SQL_ERROR);
  // The record is still there (the negative length did not disturb the area).
  ODBC_CHECK(Odbc.SQLGetDiagRecW(SQL_HANDLE_DBC, dbc.h, 1, state, &native, msg, 512, &msgLen), SQL_HANDLE_DBC, dbc.h);
  CHECK(fromWide(msg, msgLen).find(bad) != std::string::npos);

  SQLWCHAR wstate[6] = {0};
  SQLWCHAR wmsg[512] = {0};
  SQLSMALLINT wlen = 0;
  const SQLRETURN rc = Odbc.SQLErrorW(SQL_NULL_HENV, dbc.h, SQL_NULL_HSTMT, wstate, &native, wmsg, 512, &wlen);
  CHECK(rc == SQL_SUCCESS);
  CHECK_EQ(fromWide(wstate, 5), std::string("HY000"));
  CHECK(fromWide(wmsg, wlen).find(bad) != std::string::npos);
  CHECK(Odbc.SQLErrorW(SQL_NULL_HENV, dbc.h, SQL_NULL_HSTMT, wstate, &native, wmsg, 512, &wlen) == SQL_NO_DATA);
  CHECK(Odbc.SQLErrorW(SQL_NULL_HENV, dbc.h, SQL_NULL_HSTMT, wstate, &native, wmsg, -1, &wlen) == SQL_ERROR);
  CHECK(Odbc.SQLErrorW(SQL_NULL_HENV, SQL_NULL_HDBC, SQL_NULL_HSTMT, wstate, &native, wmsg, 512, &wlen) ==
        SQL_INVALID_HANDLE);
}
