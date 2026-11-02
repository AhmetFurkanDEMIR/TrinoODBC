// Every Unicode (*W) entry point of the driver. Each one converts its SQLWCHAR inputs to UTF-8
// with textIn(), wraps its output buffers in a TextOut with the length convention the spec assigns
// to that function, and forwards to the shared impl:: body in api_common.h so the ANSI and Unicode
// paths can never diverge in behaviour.
//
// Length conventions (ODBC spec, "Unicode Function Arguments"):
//   * characters (SQLWCHAR units): SQLConnectW, SQLDriverConnectW, SQLBrowseConnectW, SQLPrepareW,
//     SQLExecDirectW, SQLNativeSqlW, SQLGet/SetCursorNameW, SQLDescribeColW, the catalog functions,
//     SQLGetDiagRecW, SQLErrorW, SQLGetDescRecW;
//   * bytes: SQLGetInfoW, SQLColAttributeW, SQLColAttributesW, SQLGet/SetConnectAttrW,
//     SQLGet/SetStmtAttrW, SQLGet/SetDescFieldW, SQLGetDiagFieldW.
#include "odbc/api_common.h"
#include "odbc/api_internal_attrs.h"

#include "catalog/catalog.h"
#include "util/errors.h"
#include "util/log.h"
#include "util/unicode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trino_odbc {
namespace {

constexpr const char* kLog = "odbc";

/// One wide input argument (null pointer -> nullopt, distinct from "").
std::optional<std::string> wideArg(const SQLWCHAR* text, SQLSMALLINT length) {
  return textIn(text, static_cast<SQLLEN>(length));
}

/// Builds the CatalogArgs of a Unicode catalog call.
CatalogArgs makeWideArgs(const Statement& s, const SQLWCHAR* catalog, SQLSMALLINT catalogLen, const SQLWCHAR* schema,
                         SQLSMALLINT schemaLen, const SQLWCHAR* table, SQLSMALLINT tableLen,
                         const SQLWCHAR* column = nullptr, SQLSMALLINT columnLen = 0,
                         const SQLWCHAR* tableTypes = nullptr, SQLSMALLINT tableTypesLen = 0) {
  CatalogArgs args;
  args.catalog = wideArg(catalog, catalogLen);
  args.schema = wideArg(schema, schemaLen);
  args.table = wideArg(table, tableLen);
  args.column = wideArg(column, columnLen);
  args.tableTypes = wideArg(tableTypes, tableTypesLen);
  args.identifiersAreLiterals = s.metadataId == SQL_TRUE || (s.conn != nullptr && s.conn->config.metadataCaseSensitive);
  return args;
}

/// SQL text arguments may not be null (HY009); the DM usually catches this but direct loaders do not.
std::string requireSqlText(const SQLWCHAR* text, SQLINTEGER length, const char* function) {
  std::optional<std::string> sql = textIn(text, static_cast<SQLLEN>(length));
  if (!sql) {
    throw DriverError(sqlstate::kInvalidNullPointer, std::string(function) + ": StatementText must not be a null pointer");
  }
  return std::move(*sql);
}

/// ODBC 2 connection options whose value is a string pointer (the rest are integers passed by value).
bool isStringConnectOption(SQLUSMALLINT option) {
  switch (option) {
    case SQL_ATTR_CURRENT_CATALOG:
    case SQL_ATTR_TRACEFILE:
    case SQL_ATTR_TRANSLATE_LIB:
      return true;
    default:
      return false;
  }
}

/// ODBC 2 options carry integer values in the pointer-sized argument itself.
SQLPOINTER optionValueAsPointer(SQLULEN value) {
  return reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(value));
}

}  // namespace
}  // namespace trino_odbc

using namespace trino_odbc;

// SQLColAttributeW's NumericAttribute argument is SQLPOINTER on 32-bit Windows and SQLLEN* elsewhere;
// the definition must match the driver-manager header or C++ sees a conflicting declaration.
#if defined(_WIN32) && !defined(_WIN64)
using TrinoNumAttrPtr = SQLPOINTER;
#else
using TrinoNumAttrPtr = SQLLEN*;
#endif

extern "C" {

// ---- Connection ----------------------------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLConnectW(SQLHDBC hdbc, SQLWCHAR* szDSN, SQLSMALLINT cbDSN, SQLWCHAR* szUID,
                                             SQLSMALLINT cbUID, SQLWCHAR* szAuthStr, SQLSMALLINT cbAuthStr) {
  return guardDbc(hdbc, "SQLConnectW", [&](Connection& c) -> SQLRETURN {
    return impl::connect(c, wideArg(szDSN, cbDSN), wideArg(szUID, cbUID), wideArg(szAuthStr, cbAuthStr));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC hdbc, SQLHWND hwnd, SQLWCHAR* szConnStrIn,
                                                   SQLSMALLINT cbConnStrIn, SQLWCHAR* szConnStrOut,
                                                   SQLSMALLINT cbConnStrOutMax, SQLSMALLINT* pcbConnStrOut,
                                                   SQLUSMALLINT fDriverCompletion) {
  return guardDbc(hdbc, "SQLDriverConnectW", [&](Connection& c) -> SQLRETURN {
    std::string in = wideArg(szConnStrIn, cbConnStrIn).value_or(std::string());
    trino_odbc::TextOut out(szConnStrOut, static_cast<SQLLEN>(cbConnStrOutMax), pcbConnStrOut, false);
    return impl::driverConnect(c, hwnd, std::move(in), out, fDriverCompletion);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLBrowseConnectW(SQLHDBC hdbc, SQLWCHAR* szConnStrIn, SQLSMALLINT cbConnStrIn,
                                                   SQLWCHAR* szConnStrOut, SQLSMALLINT cbConnStrOutMax,
                                                   SQLSMALLINT* pcbConnStrOut) {
  return guardDbc(hdbc, "SQLBrowseConnectW", [&](Connection& c) -> SQLRETURN {
    std::string in = wideArg(szConnStrIn, cbConnStrIn).value_or(std::string());
    trino_odbc::TextOut out(szConnStrOut, static_cast<SQLLEN>(cbConnStrOutMax), pcbConnStrOut, false);
    return impl::browseConnect(c, std::move(in), out);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetInfoW(SQLHDBC hdbc, SQLUSMALLINT fInfoType, SQLPOINTER rgbInfoValue,
                                             SQLSMALLINT cbInfoValueMax, SQLSMALLINT* pcbInfoValue) {
  return guardDbc(hdbc, "SQLGetInfoW", [&](Connection& c) -> SQLRETURN {
    return impl::getInfo(c, fInfoType, rgbInfoValue, cbInfoValueMax, pcbInfoValue, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC hdbc, SQLWCHAR* szSqlStrIn, SQLINTEGER cbSqlStrIn,
                                               SQLWCHAR* szSqlStr, SQLINTEGER cbSqlStrMax, SQLINTEGER* pcbSqlStr) {
  return guardDbc(hdbc, "SQLNativeSqlW", [&](Connection& c) -> SQLRETURN {
    std::string in = requireSqlText(szSqlStrIn, cbSqlStrIn, "SQLNativeSqlW");
    trino_odbc::TextOut out(szSqlStr, static_cast<SQLLEN>(cbSqlStrMax), pcbSqlStr, false);
    return impl::nativeSql(c, std::move(in), out);
  });
}

// ---- Connection attributes / ODBC 2 options ----------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC hdbc, SQLINTEGER fAttribute, SQLPOINTER rgbValue,
                                                    SQLINTEGER cbValue) {
  return guardDbc(hdbc, "SQLSetConnectAttrW", [&](Connection& c) -> SQLRETURN {
    // String attributes arrive as SQLWCHAR text with a BYTE length (or SQL_NTS); impl converts.
    return impl::setConnectAttr(c, fAttribute, rgbValue, cbValue, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC hdbc, SQLINTEGER fAttribute, SQLPOINTER rgbValue,
                                                    SQLINTEGER cbValueMax, SQLINTEGER* pcbValue) {
  return guardDbc(hdbc, "SQLGetConnectAttrW", [&](Connection& c) -> SQLRETURN {
    return impl::getConnectAttr(c, fAttribute, rgbValue, cbValueMax, pcbValue, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetConnectOptionW(SQLHDBC hdbc, SQLUSMALLINT fOption, SQLULEN vParam) {
  return guardDbc(hdbc, "SQLSetConnectOptionW", [&](Connection& c) -> SQLRETURN {
    // ODBC 2 convention: string options pass a pointer to a NUL-terminated string in vParam, every
    // other option passes the integer value itself.
    if (isStringConnectOption(fOption)) {
      return impl::setConnectAttr(c, static_cast<SQLINTEGER>(fOption), optionValueAsPointer(vParam), SQL_NTS, true);
    }
    return impl::setConnectAttr(c, static_cast<SQLINTEGER>(fOption), optionValueAsPointer(vParam), SQL_IS_UINTEGER,
                                true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetConnectOptionW(SQLHDBC hdbc, SQLUSMALLINT fOption, SQLPOINTER pvParam) {
  return guardDbc(hdbc, "SQLGetConnectOptionW", [&](Connection& c) -> SQLRETURN {
    // ODBC 2 string options are returned into a caller buffer of SQL_MAX_OPTION_STRING_LENGTH bytes.
    const SQLINTEGER bufLen = isStringConnectOption(fOption) ? SQL_MAX_OPTION_STRING_LENGTH : 0;
    return impl::getConnectAttr(c, static_cast<SQLINTEGER>(fOption), pvParam, bufLen, nullptr, true);
  });
}

// ---- Statement attributes -------------------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT hstmt, SQLINTEGER fAttribute, SQLPOINTER rgbValue,
                                                 SQLINTEGER cbValueMax) {
  return guardStmt(hstmt, "SQLSetStmtAttrW", [&](Statement& s) -> SQLRETURN {
    return impl::setStmtAttr(s, fAttribute, rgbValue, cbValueMax, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT hstmt, SQLINTEGER fAttribute, SQLPOINTER rgbValue,
                                                 SQLINTEGER cbValueMax, SQLINTEGER* pcbValue) {
  return guardStmt(hstmt, "SQLGetStmtAttrW", [&](Statement& s) -> SQLRETURN {
    return impl::getStmtAttr(s, fAttribute, rgbValue, cbValueMax, pcbValue, true);
  });
}

// ---- Statement text / cursor name ----------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLPrepareW(SQLHSTMT hstmt, SQLWCHAR* szSqlStr, SQLINTEGER cbSqlStr) {
  return guardStmt(hstmt, "SQLPrepareW", [&](Statement& s) -> SQLRETURN {
    return impl::prepare(s, requireSqlText(szSqlStr, cbSqlStr, "SQLPrepareW"));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT hstmt, SQLWCHAR* szSqlStr, SQLINTEGER cbSqlStr) {
  return guardStmt(hstmt, "SQLExecDirectW", [&](Statement& s) -> SQLRETURN {
    return impl::execDirect(s, requireSqlText(szSqlStr, cbSqlStr, "SQLExecDirectW"));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT hstmt, SQLWCHAR* szCursor, SQLSMALLINT cbCursorMax,
                                                   SQLSMALLINT* pcbCursor) {
  return guardStmt(hstmt, "SQLGetCursorNameW", [&](Statement& s) -> SQLRETURN {
    trino_odbc::TextOut out(szCursor, static_cast<SQLLEN>(cbCursorMax), pcbCursor, false);
    return impl::getCursorName(s, out);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT hstmt, SQLWCHAR* szCursor, SQLSMALLINT cbCursor) {
  return guardStmt(hstmt, "SQLSetCursorNameW", [&](Statement& s) -> SQLRETURN {
    std::optional<std::string> name = wideArg(szCursor, cbCursor);
    if (!name) throw DriverError(sqlstate::kInvalidNullPointer, "SQLSetCursorNameW: CursorName must not be a null pointer");
    return impl::setCursorName(s, std::move(*name));
  });
}

// ---- Result metadata ------------------------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLWCHAR* szColName,
                                                 SQLSMALLINT cbColNameMax, SQLSMALLINT* pcbColName,
                                                 SQLSMALLINT* pfSqlType, SQLULEN* pcbColDef, SQLSMALLINT* pibScale,
                                                 SQLSMALLINT* pfNullable) {
  return guardStmt(hstmt, "SQLDescribeColW", [&](Statement& s) -> SQLRETURN {
    trino_odbc::TextOut name(szColName, static_cast<SQLLEN>(cbColNameMax), pcbColName, false);
    return impl::describeCol(s, icol, name, pfSqlType, pcbColDef, pibScale, pfNullable);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT hstmt, SQLUSMALLINT iCol, SQLUSMALLINT iField,
                                                  SQLPOINTER pCharAttr, SQLSMALLINT cbCharAttrMax,
                                                  SQLSMALLINT* pcbCharAttr, TrinoNumAttrPtr pNumAttr) {
  return guardStmt(hstmt, "SQLColAttributeW", [&](Statement& s) -> SQLRETURN {
    return impl::colAttribute(s, iCol, iField, pCharAttr, cbCharAttrMax, pcbCharAttr,
                              static_cast<SQLLEN*>(pNumAttr), true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColAttributesW(SQLHSTMT hstmt, SQLUSMALLINT icol, SQLUSMALLINT fDescType,
                                                   SQLPOINTER rgbDesc, SQLSMALLINT cbDescMax, SQLSMALLINT* pcbDesc,
                                                   SQLLEN* pfDesc) {
  return guardStmt(hstmt, "SQLColAttributesW", [&](Statement& s) -> SQLRETURN {
    // ODBC 2 SQL_COLUMN_* identifiers are understood by impl::colAttribute alongside SQL_DESC_*.
    return impl::colAttribute(s, icol, fDescType, rgbDesc, cbDescMax, pcbDesc, pfDesc, true);
  });
}

// ---- Catalog functions ----------------------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLTablesW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                            SQLWCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLWCHAR* szTableName,
                                            SQLSMALLINT cbTableName, SQLWCHAR* szTableType, SQLSMALLINT cbTableType) {
  return guardStmt(hstmt, "SQLTablesW", [&](Statement& s) -> SQLRETURN {
    return impl::tables(s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName,
                                        cbTableName, nullptr, 0, szTableType, cbTableType));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColumnsW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                             SQLWCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLWCHAR* szTableName,
                                             SQLSMALLINT cbTableName, SQLWCHAR* szColumnName,
                                             SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLColumnsW", [&](Statement& s) -> SQLRETURN {
    return impl::columns(s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName,
                                         cbTableName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLPrimaryKeysW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                 SQLWCHAR* szSchemaName, SQLSMALLINT cbSchemaName,
                                                 SQLWCHAR* szTableName, SQLSMALLINT cbTableName) {
  return guardStmt(hstmt, "SQLPrimaryKeysW", [&](Statement& s) -> SQLRETURN {
    return impl::primaryKeys(
        s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLForeignKeysW(SQLHSTMT hstmt, SQLWCHAR* szPkCatalogName,
                                                 SQLSMALLINT cbPkCatalogName, SQLWCHAR* szPkSchemaName,
                                                 SQLSMALLINT cbPkSchemaName, SQLWCHAR* szPkTableName,
                                                 SQLSMALLINT cbPkTableName, SQLWCHAR* szFkCatalogName,
                                                 SQLSMALLINT cbFkCatalogName, SQLWCHAR* szFkSchemaName,
                                                 SQLSMALLINT cbFkSchemaName, SQLWCHAR* szFkTableName,
                                                 SQLSMALLINT cbFkTableName) {
  return guardStmt(hstmt, "SQLForeignKeysW", [&](Statement& s) -> SQLRETURN {
    return impl::foreignKeys(s,
                             makeWideArgs(s, szPkCatalogName, cbPkCatalogName, szPkSchemaName, cbPkSchemaName,
                                          szPkTableName, cbPkTableName),
                             makeWideArgs(s, szFkCatalogName, cbFkCatalogName, szFkSchemaName, cbFkSchemaName,
                                          szFkTableName, cbFkTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLStatisticsW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                SQLWCHAR* szSchemaName, SQLSMALLINT cbSchemaName,
                                                SQLWCHAR* szTableName, SQLSMALLINT cbTableName, SQLUSMALLINT fUnique,
                                                SQLUSMALLINT fAccuracy) {
  return guardStmt(hstmt, "SQLStatisticsW", [&](Statement& s) -> SQLRETURN {
    return impl::statistics(
        s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName),
        fUnique, fAccuracy);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSpecialColumnsW(SQLHSTMT hstmt, SQLUSMALLINT fColType, SQLWCHAR* szCatalogName,
                                                    SQLSMALLINT cbCatalogName, SQLWCHAR* szSchemaName,
                                                    SQLSMALLINT cbSchemaName, SQLWCHAR* szTableName,
                                                    SQLSMALLINT cbTableName, SQLUSMALLINT fScope,
                                                    SQLUSMALLINT fNullable) {
  return guardStmt(hstmt, "SQLSpecialColumnsW", [&](Statement& s) -> SQLRETURN {
    return impl::specialColumns(
        s, fColType,
        makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName), fScope,
        fNullable);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLProceduresW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName, SQLSMALLINT cbCatalogName,
                                                SQLWCHAR* szSchemaName, SQLSMALLINT cbSchemaName, SQLWCHAR* szProcName,
                                                SQLSMALLINT cbProcName) {
  return guardStmt(hstmt, "SQLProceduresW", [&](Statement& s) -> SQLRETURN {
    return impl::procedures(
        s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szProcName, cbProcName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLProcedureColumnsW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName,
                                                      SQLSMALLINT cbCatalogName, SQLWCHAR* szSchemaName,
                                                      SQLSMALLINT cbSchemaName, SQLWCHAR* szProcName,
                                                      SQLSMALLINT cbProcName, SQLWCHAR* szColumnName,
                                                      SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLProcedureColumnsW", [&](Statement& s) -> SQLRETURN {
    return impl::procedureColumns(s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName,
                                                  szProcName, cbProcName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLTablePrivilegesW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName,
                                                     SQLSMALLINT cbCatalogName, SQLWCHAR* szSchemaName,
                                                     SQLSMALLINT cbSchemaName, SQLWCHAR* szTableName,
                                                     SQLSMALLINT cbTableName) {
  return guardStmt(hstmt, "SQLTablePrivilegesW", [&](Statement& s) -> SQLRETURN {
    return impl::tablePrivileges(
        s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName, szTableName, cbTableName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLColumnPrivilegesW(SQLHSTMT hstmt, SQLWCHAR* szCatalogName,
                                                      SQLSMALLINT cbCatalogName, SQLWCHAR* szSchemaName,
                                                      SQLSMALLINT cbSchemaName, SQLWCHAR* szTableName,
                                                      SQLSMALLINT cbTableName, SQLWCHAR* szColumnName,
                                                      SQLSMALLINT cbColumnName) {
  return guardStmt(hstmt, "SQLColumnPrivilegesW", [&](Statement& s) -> SQLRETURN {
    return impl::columnPrivileges(s, makeWideArgs(s, szCatalogName, cbCatalogName, szSchemaName, cbSchemaName,
                                                  szTableName, cbTableName, szColumnName, cbColumnName));
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT hstmt, SQLSMALLINT fSqlType) {
  return guardStmt(hstmt, "SQLGetTypeInfoW",
                   [&](Statement& s) -> SQLRETURN { return impl::getTypeInfo(s, fSqlType); });
}

// ---- Diagnostics ---------------------------------------------------------------------------------
// The diagnostic readers never run through a guard: a guard would clear the very records the
// application is about to read. impl::getDiagRec / getDiagField validate and lock on their own;
// the try/catch only keeps a std::bad_alloc from escaping the C ABI.

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT fHandleType, SQLHANDLE handle, SQLSMALLINT iRecord,
                                                SQLWCHAR* szSqlState, SQLINTEGER* pfNativeError, SQLWCHAR* szErrorMsg,
                                                SQLSMALLINT cbErrorMsgMax, SQLSMALLINT* pcbErrorMsg) {
  // Same as the ANSI twin: a negative BufferLength is an error (HY090 cannot be posted without
  // disturbing the area being read).
  if (cbErrorMsgMax < 0) return SQL_ERROR;
  try {
    // SQLSTATE buffers are always 5 characters plus terminator; no length is reported for them.
    trino_odbc::TextOut sqlstate(szSqlState, szSqlState != nullptr ? SQL_SQLSTATE_SIZE + 1 : 0,
                     static_cast<SQLSMALLINT*>(nullptr), false);
    trino_odbc::TextOut message(szErrorMsg, static_cast<SQLLEN>(cbErrorMsgMax), pcbErrorMsg, false);
    return impl::getDiagRec(fHandleType, handle, iRecord, sqlstate, pfNativeError, message);
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "SQLGetDiagRecW failed: %s", e.what());
    return SQL_ERROR;
  }
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT fHandleType, SQLHANDLE handle, SQLSMALLINT iRecord,
                                                  SQLSMALLINT fDiagField, SQLPOINTER rgbDiagInfo,
                                                  SQLSMALLINT cbDiagInfoMax, SQLSMALLINT* pcbDiagInfo) {
  try {
    return impl::getDiagField(fHandleType, handle, iRecord, fDiagField, rgbDiagInfo, cbDiagInfoMax, pcbDiagInfo, true);
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "SQLGetDiagFieldW failed: %s", e.what());
    return SQL_ERROR;
  }
}

/// ODBC 2 SQLErrorW. Shares the record cursor of SQLError (impl::odbc2Error) so applications get
/// identical sequencing whichever flavour they call; the message is transcoded from UTF-8 directly
/// into SQLWCHARs, never through the ANSI code page.
TRINO_ODBC_API SQLRETURN SQL_API SQLErrorW(SQLHENV henv, SQLHDBC hdbc, SQLHSTMT hstmt, SQLWCHAR* szSqlState,
                                           SQLINTEGER* pfNativeError, SQLWCHAR* szErrorMsg, SQLSMALLINT cbErrorMsgMax,
                                           SQLSMALLINT* pcbErrorMsg) {
  try {
    return impl::odbc2Error(henv, hdbc, hstmt, szSqlState, pfNativeError, szErrorMsg, cbErrorMsgMax, pcbErrorMsg,
                            /*wide=*/true);
  } catch (const std::exception& e) {
    TRINO_LOG_ERROR(kLog, "SQLErrorW failed: %s", e.what());
    return SQL_ERROR;
  }
}

// ---- Descriptors ---------------------------------------------------------------------------------

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDescFieldW(SQLHDESC hdesc, SQLSMALLINT iRecord, SQLSMALLINT iField,
                                                  SQLPOINTER rgbValue, SQLINTEGER cbValueMax, SQLINTEGER* pcbValue) {
  return guardDesc(hdesc, "SQLGetDescFieldW", [&](Descriptor& d) -> SQLRETURN {
    return impl::getDescField(d, iRecord, iField, rgbValue, cbValueMax, pcbValue, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLSetDescFieldW(SQLHDESC hdesc, SQLSMALLINT iRecord, SQLSMALLINT iField,
                                                  SQLPOINTER rgbValue, SQLINTEGER cbValue) {
  return guardDesc(hdesc, "SQLSetDescFieldW", [&](Descriptor& d) -> SQLRETURN {
    return impl::setDescField(d, iRecord, iField, rgbValue, cbValue, true);
  });
}

TRINO_ODBC_API SQLRETURN SQL_API SQLGetDescRecW(SQLHDESC hdesc, SQLSMALLINT iRecord, SQLWCHAR* szName,
                                                SQLSMALLINT cbNameMax, SQLSMALLINT* pcbName, SQLSMALLINT* pfType,
                                                SQLSMALLINT* pfSubType, SQLLEN* pLength, SQLSMALLINT* pPrecision,
                                                SQLSMALLINT* pScale, SQLSMALLINT* pNullable) {
  return guardDesc(hdesc, "SQLGetDescRecW", [&](Descriptor& d) -> SQLRETURN {
    trino_odbc::TextOut name(szName, static_cast<SQLLEN>(cbNameMax), pcbName, false);
    return impl::getDescRec(d, iRecord, name, pfType, pfSubType, pLength, pPrecision, pScale, pNullable);
  });
}

/// SQLSetDescRec has no string arguments, so the driver-manager headers declare no W variant; it is
/// still exported (some managers probe for it) with exactly the ANSI signature.
TRINO_ODBC_API SQLRETURN SQL_API SQLSetDescRecW(SQLHDESC hdesc, SQLSMALLINT iRecord, SQLSMALLINT fType,
                                                SQLSMALLINT fSubType, SQLLEN cbLength, SQLSMALLINT ibPrecision,
                                                SQLSMALLINT ibScale, SQLPOINTER rgbData, SQLLEN* pcbStringLength,
                                                SQLLEN* pIndicator) {
  return guardDesc(hdesc, "SQLSetDescRecW", [&](Descriptor& d) -> SQLRETURN {
    return impl::setDescRec(d, iRecord, fType, fSubType, cbLength, ibPrecision, ibScale, rgbData, pcbStringLength,
                            pIndicator);
  });
}

}  // extern "C"
