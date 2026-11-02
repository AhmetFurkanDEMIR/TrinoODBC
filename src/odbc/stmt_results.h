#pragma once
// Private helpers of the statement / result-set entry points (api_statement.cpp, api_results.cpp)
// that other api_*.cpp files may reuse.
#include "odbc/api_common.h"

namespace trino_odbc {

// Owned by api_common.cpp: drops the SQLError walk position of a diagnostic area.
namespace diag_cursor {
void reset(const Diag& d);
}

/// True when the application declared ODBC 2 behaviour for the environment of `c`
/// (SQLAllocEnv, or SQL_ATTR_ODBC_VERSION = SQL_OV_ODBC2). api_results.cpp.
bool isOdbc2Application(const Connection* c);

/// SQL type code as the application's ODBC version knows it: an ODBC 2 application receives
/// SQL_DATE / SQL_TIME / SQL_TIMESTAMP (9/10/11) instead of SQL_TYPE_DATE / SQL_TYPE_TIME /
/// SQL_TYPE_TIMESTAMP (91/92/93); every other code is returned unchanged. api_results.cpp.
SQLSMALLINT sqlTypeForApplication(const Connection* c, SQLSMALLINT sqlType);

namespace impl {
/// SQLFreeHandle(SQL_HANDLE_STMT) without calling the exported name (which, on Linux, may bind to
/// the driver manager's function of the same name). Cancels in-flight work before it waits for the
/// statement. api_statement.cpp.
SQLRETURN freeStatementHandle(SQLHSTMT h);
}  // namespace impl

}  // namespace trino_odbc
