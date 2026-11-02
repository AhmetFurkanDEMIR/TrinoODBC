// Unit tests for core/diag: record ordering, message formatting, Trino error mapping, dynamic
// function classification and return-code merging.
#include "unit/test_framework.h"

#include "core/diag.h"
#include "util/errors.h"

#include <stdexcept>
#include <string>

using namespace trino_odbc;

TEST_CASE("diag: empty area and clear") {
  Diag d;
  CHECK(d.empty());
  CHECK_EQ(d.count(), size_t(0));
  CHECK(d.record(1) == nullptr);
  CHECK(d.record(0) == nullptr);
  d.addError("HY000", "boom");
  d.rowCount = 5;
  d.dynamicFunction = "INSERT";
  d.dynamicFunctionCode = SQL_DIAG_INSERT;
  d.returnCode = SQL_ERROR;
  d.clear();
  CHECK(d.empty());
  CHECK_EQ(d.rowCount, SQLLEN(-1));
  CHECK_EQ(d.cursorRowCount, SQLLEN(-1));
  CHECK_EQ(d.dynamicFunction, std::string(""));
  CHECK_EQ(d.dynamicFunctionCode, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
  CHECK_EQ(d.returnCode, SQLRETURN(SQL_SUCCESS));
}

TEST_CASE("diag: addError / addWarning return codes and formatting") {
  Diag d;
  CHECK_EQ(d.addWarning("01004", "String data, right truncated"), SQLRETURN(SQL_SUCCESS_WITH_INFO));
  CHECK_EQ(d.addError("HY010", "Function sequence error", 42), SQLRETURN(SQL_ERROR));
  CHECK_EQ(d.count(), size_t(2));
  const DiagRecord* r1 = d.record(1);
  REQUIRE(r1 != nullptr);
  CHECK_EQ(r1->sqlstate, std::string("HY010"));
  CHECK_EQ(r1->message, std::string("[Trino][ODBC Driver] Function sequence error"));
  CHECK_EQ(r1->native, SQLINTEGER(42));
  CHECK(!r1->isWarning());
  const DiagRecord* r2 = d.record(2);
  REQUIRE(r2 != nullptr);
  CHECK_EQ(r2->sqlstate, std::string("01004"));
  CHECK(r2->isWarning());
  CHECK_EQ(r2->message, std::string("[Trino][ODBC Driver] String data, right truncated"));
  CHECK(d.record(3) == nullptr);
}

TEST_CASE("diag: errors are ordered before warnings, insertion order within a class") {
  Diag d;
  d.addWarning("01000", "w1");
  d.addWarning("01S02", "w2");
  d.addError("22003", "e1");
  d.addWarning("01004", "w3");
  d.addError("HY000", "e2");
  REQUIRE_EQ(d.count(), size_t(5));
  CHECK_EQ(d.records()[0].sqlstate, std::string("22003"));
  CHECK_EQ(d.records()[1].sqlstate, std::string("HY000"));
  CHECK_EQ(d.records()[2].sqlstate, std::string("01000"));
  CHECK_EQ(d.records()[3].sqlstate, std::string("01S02"));
  CHECK_EQ(d.records()[4].sqlstate, std::string("01004"));
}

TEST_CASE("diag: row/column numbers and header names propagate") {
  Diag d;
  d.serverName = "trino.example.com";
  d.connectionName = "MyDSN";
  d.addError("22018", "bad value", 0, 3, 7);
  const DiagRecord* r = d.record(1);
  REQUIRE(r != nullptr);
  CHECK_EQ(r->rowNumber, SQLLEN(3));
  CHECK_EQ(r->columnNumber, SQLINTEGER(7));
  CHECK_EQ(r->serverName, std::string("trino.example.com"));
  CHECK_EQ(r->connectionName, std::string("MyDSN"));
  d.addWarning("01000", "generic");
  // Errors outrank warnings whatever their row: the row-3 error stays first.
  r = d.record(1);
  REQUIRE(r != nullptr);
  CHECK_EQ(r->sqlstate, std::string("22018"));
  CHECK_EQ(r->rowNumber, SQLLEN(3));
  r = d.record(2);
  REQUIRE(r != nullptr);
  CHECK_EQ(r->sqlstate, std::string("01000"));
  CHECK_EQ(r->rowNumber, SQLLEN(SQL_NO_ROW_NUMBER));
  CHECK_EQ(r->columnNumber, SQLINTEGER(SQL_NO_COLUMN_NUMBER));
}

TEST_CASE("diag: records are ordered by severity, then row number (block fetch)") {
  // ROW_ARRAY_SIZE = 3: row 1 truncates (01004), row 2 overflows (22003) and gets its 01S01.
  // ODBC: errors precede warnings; within one class the row rule applies (row unknown, then no
  // row, then ascending row numbers), and equal keys keep insertion order.
  Diag d;
  d.addWarning("01004", "String data, right truncated", 0, 1, 1);
  d.addError("22003", "Numeric value out of range", 0, 2, 1);
  d.addWarning("01S01", "Error in row", 0, 2);
  d.addError("HY000", "no row", 0);
  d.addWarning("01000", "unknown row", 0, SQL_ROW_NUMBER_UNKNOWN);
  d.addError("22018", "row 1 error", 0, 1, 2);
  d.addError("HY001", "unknown row error", 0, SQL_ROW_NUMBER_UNKNOWN);
  d.addWarning("01S07", "row 1 second warning", 0, 1, 2);
  REQUIRE_EQ(d.count(), size_t(8));
  CHECK_EQ(d.records()[0].sqlstate, std::string("HY001"));  // errors: row number unknown first
  CHECK_EQ(d.records()[1].sqlstate, std::string("HY000"));  // then errors without a row
  CHECK_EQ(d.records()[2].sqlstate, std::string("22018"));  // then row 1, row 2
  CHECK_EQ(d.records()[3].sqlstate, std::string("22003"));
  CHECK_EQ(d.records()[3].rowNumber, SQLLEN(2));
  CHECK_EQ(d.records()[4].sqlstate, std::string("01000"));  // warnings: row number unknown first
  CHECK_EQ(d.records()[5].sqlstate, std::string("01004"));  // row 1, in insertion order
  CHECK_EQ(d.records()[6].sqlstate, std::string("01S07"));
  CHECK_EQ(d.records()[7].sqlstate, std::string("01S01"));  // row 2
  CHECK_EQ(d.records()[7].rowNumber, SQLLEN(2));
}

TEST_CASE("diag: add() with a preformatted record and malformed SQLSTATE") {
  Diag d;
  DiagRecord rec;
  rec.sqlstate = "HY0";
  rec.message = "raw";
  CHECK_EQ(d.add(rec), SQLRETURN(SQL_ERROR));
  CHECK_EQ(d.record(1)->sqlstate.size(), size_t(5));
  CHECK_EQ(d.record(1)->message, std::string("raw"));
  DiagRecord empty;
  empty.message = "no state";
  d.add(empty);
  CHECK_EQ(d.record(2)->sqlstate, std::string("HY000"));
}

TEST_CASE("diag: message formatting helpers") {
  CHECK_EQ(Diag::formatDriverMessage("text"), std::string("[Trino][ODBC Driver] text"));
  CHECK_EQ(Diag::formatServerMessage("line 1:8: Column 'x' cannot be resolved"),
           std::string("[Trino][ODBC Driver][Trino] line 1:8: Column 'x' cannot be resolved"));
  CHECK_EQ(Diag::formatDriverMessage(""), std::string("[Trino][ODBC Driver] "));
}

TEST_CASE("diag: server errors carry the [Trino] component and the native error code") {
  Diag d;
  TrinoQueryError e("42S02", "Table 'tpch.tiny.nope' does not exist", 46, "TABLE_NOT_FOUND", "USER_ERROR",
                    "20240101_000000_00001_abcde", 1, 15);
  CHECK_EQ(d.addServerError(e), SQLRETURN(SQL_ERROR));
  const DiagRecord* r = d.record(1);
  REQUIRE(r != nullptr);
  CHECK_EQ(r->sqlstate, std::string("42S02"));
  CHECK_EQ(r->native, SQLINTEGER(46));
  CHECK_EQ(r->message, std::string("[Trino][ODBC Driver][Trino] Table 'tpch.tiny.nope' does not exist"));
  // Missing SQLSTATE falls back to the error-name table.
  TrinoQueryError noState("", "Division by zero", 65545, "DIVISION_BY_ZERO", "USER_ERROR", "q");
  d.addServerError(noState);
  CHECK_EQ(d.record(2)->sqlstate, std::string("22012"));
  CHECK_EQ(d.record(2)->native, SQLINTEGER(65545));
}

TEST_CASE("diag: addException variants") {
  Diag d;
  DriverError e("HY090", "Invalid string or buffer length", 7);
  e.atRow(2).atColumn(4);
  CHECK_EQ(d.addException(e), SQLRETURN(SQL_ERROR));
  const DiagRecord* r = d.record(1);
  REQUIRE(r != nullptr);
  CHECK_EQ(r->sqlstate, std::string("HY090"));
  CHECK_EQ(r->native, SQLINTEGER(7));
  CHECK_EQ(r->rowNumber, SQLLEN(2));
  CHECK_EQ(r->columnNumber, SQLINTEGER(4));
  CHECK_EQ(r->message, std::string("[Trino][ODBC Driver] Invalid string or buffer length"));

  DriverError w("01S02", "Option value changed");
  CHECK_EQ(d.addException(w), SQLRETURN(SQL_SUCCESS_WITH_INFO));

  CancelledError c;
  CHECK_EQ(d.addException(c), SQLRETURN(SQL_ERROR));
  HttpError h("08S01", "HTTP 500: oops", 500, "http://h/v1/statement", 0);
  d.addException(h);
  std::runtime_error generic("out of cheese");
  CHECK_EQ(d.addException(generic), SQLRETURN(SQL_ERROR));
  // Errors outrank the warning; among errors the no-row records come before the row-2 record, in
  // insertion order (ODBC "Sequence of Status Records").
  REQUIRE_EQ(d.count(), size_t(5));
  CHECK_EQ(d.record(1)->sqlstate, std::string("HY008"));
  CHECK_EQ(d.record(2)->sqlstate, std::string("08S01"));
  CHECK_EQ(d.record(2)->native, SQLINTEGER(500));
  CHECK_EQ(d.record(3)->sqlstate, std::string("HY000"));
  CHECK_EQ(d.record(3)->message, std::string("[Trino][ODBC Driver] out of cheese"));
  CHECK_EQ(d.record(4)->sqlstate, std::string("HY090"));
  CHECK_EQ(d.record(5)->sqlstate, std::string("01S02"));
  // The warning went to the end.
  CHECK_EQ(d.record(5)->sqlstate, std::string("01S02"));
}

TEST_CASE("diag: Trino error name -> SQLSTATE table") {
  auto st = [](const char* name, const char* type = "USER_ERROR") {
    return std::string(Diag::sqlstateForTrinoError(name, type));
  };
  CHECK_EQ(st("SYNTAX_ERROR"), std::string("42000"));
  CHECK_EQ(st("TABLE_NOT_FOUND"), std::string("42S02"));
  CHECK_EQ(st("COLUMN_NOT_FOUND"), std::string("42S22"));
  CHECK_EQ(st("SCHEMA_NOT_FOUND"), std::string("3F000"));
  CHECK_EQ(st("CATALOG_NOT_FOUND"), std::string("3D000"));
  CHECK_EQ(st("PERMISSION_DENIED"), std::string("42501"));
  CHECK_EQ(st("USER_CANCELED"), std::string("HY008"));
  CHECK_EQ(st("EXCEEDED_TIME_LIMIT", "INSUFFICIENT_RESOURCES"), std::string("HYT00"));
  CHECK_EQ(st("EXCEEDED_MEMORY_LIMIT", "INSUFFICIENT_RESOURCES"), std::string("57014"));
  CHECK_EQ(st("EXCEEDED_CPU_LIMIT", "INSUFFICIENT_RESOURCES"), std::string("57014"));
  CHECK_EQ(st("DIVISION_BY_ZERO"), std::string("22012"));
  CHECK_EQ(st("NUMERIC_VALUE_OUT_OF_RANGE"), std::string("22003"));
  CHECK_EQ(st("INVALID_CAST_ARGUMENT"), std::string("22018"));
  CHECK_EQ(st("INVALID_FUNCTION_ARGUMENT"), std::string("22023"));
  CHECK_EQ(st("TYPE_MISMATCH"), std::string("42804"));
  CHECK_EQ(st("AMBIGUOUS_NAME"), std::string("42702"));
  CHECK_EQ(st("MISSING_CATALOG_NAME"), std::string("42000"));
  CHECK_EQ(st("INVALID_ORDER_BY"), std::string("42000"));
  CHECK_EQ(st("INVALID_SOMETHING_NEW", ""), std::string("42000"));
  CHECK_EQ(st("INVALID_SOMETHING_INTERNAL", "INTERNAL_ERROR"), std::string("HY000"));
  CHECK_EQ(st("GENERIC_INTERNAL_ERROR", "INTERNAL_ERROR"), std::string("HY000"));
  CHECK_EQ(st("INTERNAL_ERROR", "INTERNAL_ERROR"), std::string("HY000"));
  CHECK_EQ(st("SOMETHING_ODD", "INSUFFICIENT_RESOURCES"), std::string("HY001"));
  CHECK_EQ(st("CLUSTER_OUT_OF_MEMORY", "INSUFFICIENT_RESOURCES"), std::string("HY001"));
  CHECK_EQ(st("EXTERNAL", "EXTERNAL"), std::string("HY000"));
  CHECK_EQ(st("NOT_SUPPORTED"), std::string("0A000"));
  CHECK_EQ(st("ALREADY_EXISTS"), std::string("42S01"));
  CHECK_EQ(st("COLUMN_ALREADY_EXISTS"), std::string("42S21"));
  CHECK_EQ(st("UNKNOWN_NAME_XYZ"), std::string("HY000"));
  CHECK_EQ(st("", ""), std::string("HY000"));
  // Case and whitespace tolerant.
  CHECK_EQ(st(" table_not_found "), std::string("42S02"));
}

TEST_CASE("diag: classifyDynamicFunction") {
  std::string name;
  SQLINTEGER code = -99;
  auto classify = [&](const char* sql) {
    Diag::classifyDynamicFunction(sql, name, code);
  };
  classify("SELECT 1");
  CHECK_EQ(name, std::string("SELECT CURSOR"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_SELECT_CURSOR));
  classify("  \n\t-- comment\n/* block\ncomment */ with x as (select 1) select * from x");
  CHECK_EQ(name, std::string("SELECT CURSOR"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_SELECT_CURSOR));
  classify("(SELECT 1) UNION (SELECT 2)");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_SELECT_CURSOR));
  classify("show tables");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_SELECT_CURSOR));
  classify("VALUES (1, 2)");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_SELECT_CURSOR));
  classify("insert into t values (1)");
  CHECK_EQ(name, std::string("INSERT"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_INSERT));
  classify("UPDATE t SET a = 1");
  CHECK_EQ(name, std::string("UPDATE WHERE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UPDATE_WHERE));
  classify("DELETE FROM t");
  CHECK_EQ(name, std::string("DELETE WHERE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_DELETE_WHERE));
  classify("MERGE INTO t USING s ON 1=1 WHEN MATCHED THEN DELETE");
  CHECK_EQ(name, std::string("MERGE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
  classify("CREATE TABLE t (a int)");
  CHECK_EQ(name, std::string("CREATE TABLE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_CREATE_TABLE));
  classify("create or replace view v as select 1");
  CHECK_EQ(name, std::string("CREATE VIEW"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_CREATE_VIEW));
  classify("CREATE MATERIALIZED VIEW mv AS SELECT 1");
  CHECK_EQ(name, std::string("CREATE MATERIALIZED VIEW"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
  classify("CREATE SCHEMA s");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_CREATE_SCHEMA));
  classify("DROP TABLE IF EXISTS t");
  CHECK_EQ(name, std::string("DROP TABLE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_DROP_TABLE));
  classify("DROP VIEW v");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_DROP_VIEW));
  classify("DROP SCHEMA s");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_DROP_SCHEMA));
  classify("ALTER TABLE t ADD COLUMN b int");
  CHECK_EQ(name, std::string("ALTER TABLE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_ALTER_TABLE));
  classify("ALTER SCHEMA s RENAME TO t");
  CHECK_EQ(name, std::string("ALTER SCHEMA"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
  classify("CALL system.flush_metadata_cache()");
  CHECK_EQ(name, std::string("CALL"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_CALL));
  classify("GRANT SELECT ON t TO alice");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_GRANT));
  classify("REVOKE SELECT ON t FROM alice");
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_REVOKE));
  classify("START TRANSACTION");
  CHECK_EQ(name, std::string("START TRANSACTION"));
  classify("COMMIT");
  CHECK_EQ(name, std::string("COMMIT"));
  classify("USE tpch.tiny");
  CHECK_EQ(name, std::string("USE"));
  classify("SET SESSION x = 1");
  CHECK_EQ(name, std::string("SET SESSION"));
  classify("EXECUTE IMMEDIATE 'select 1'");
  CHECK_EQ(name, std::string("EXECUTE IMMEDIATE"));
  classify("EXECUTE odbc_stmt_1 USING 1");
  CHECK_EQ(name, std::string("EXECUTE"));
  classify("");
  CHECK_EQ(name, std::string(""));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
  classify("   -- only a comment");
  CHECK_EQ(name, std::string(""));
  classify("FROBNICATE x");
  CHECK_EQ(name, std::string("FROBNICATE"));
  CHECK_EQ(code, SQLINTEGER(SQL_DIAG_UNKNOWN_STATEMENT));
}

TEST_CASE("diag: mergeReturnCode keeps the most severe code") {
  CHECK_EQ(mergeReturnCode(SQL_SUCCESS, SQL_SUCCESS), SQLRETURN(SQL_SUCCESS));
  CHECK_EQ(mergeReturnCode(SQL_SUCCESS, SQL_SUCCESS_WITH_INFO), SQLRETURN(SQL_SUCCESS_WITH_INFO));
  CHECK_EQ(mergeReturnCode(SQL_SUCCESS_WITH_INFO, SQL_SUCCESS), SQLRETURN(SQL_SUCCESS_WITH_INFO));
  CHECK_EQ(mergeReturnCode(SQL_SUCCESS_WITH_INFO, SQL_NO_DATA), SQLRETURN(SQL_NO_DATA));
  CHECK_EQ(mergeReturnCode(SQL_NO_DATA, SQL_NEED_DATA), SQLRETURN(SQL_NEED_DATA));
  CHECK_EQ(mergeReturnCode(SQL_NEED_DATA, SQL_ERROR), SQLRETURN(SQL_ERROR));
  CHECK_EQ(mergeReturnCode(SQL_ERROR, SQL_SUCCESS_WITH_INFO), SQLRETURN(SQL_ERROR));
  CHECK_EQ(mergeReturnCode(SQL_ERROR, SQL_NO_DATA), SQLRETURN(SQL_ERROR));
  CHECK_EQ(mergeReturnCode(SQL_SUCCESS, SQL_INVALID_HANDLE), SQLRETURN(SQL_INVALID_HANDLE));
  CHECK_EQ(mergeReturnCode(SQL_ERROR, SQL_INVALID_HANDLE), SQLRETURN(SQL_INVALID_HANDLE));
  CHECK_EQ(mergeReturnCode(SQL_STILL_EXECUTING, SQL_SUCCESS_WITH_INFO), SQLRETURN(SQL_STILL_EXECUTING));
}
