#pragma once
// Private helpers shared by api_attrs.cpp, api_connect.cpp, api_diag.cpp and api_unicode.cpp.
// Exported SQL* names are never called from inside the driver: on Linux the entry points are
// interposable (default visibility, no -Bsymbolic), so such a call can bind to the driver manager's
// function of the same name and hand it a driver-level handle. These internal functions are used
// instead.
#include "odbc/api_common.h"

namespace trino_odbc {
namespace impl {

/// SQL_ATTR_RESET_CONNECTION (connection returned to a driver-manager pool): rolls back an open
/// driver transaction, closes every statement's cursor, restores the session (catalog, schema,
/// path, session properties, roles, prepared statements, authorization user) to the configured
/// defaults and resets the connection attributes to their defaults (api_connect.cpp).
void resetPooledConnection(Connection& c);

/// SQLEndTran for SQL_HANDLE_ENV / SQL_HANDLE_DBC (shared by SQLEndTran and SQLTransact; api_connect.cpp).
SQLRETURN endTransaction(SQLSMALLINT handleType, SQLHANDLE handle, SQLSMALLINT completion);

/// ODBC 2 SQLError / SQLErrorW: returns the next unread record of the most specific handle given.
/// `sqlstate` (6 characters incl. terminator) and `message` are SQLCHAR buffers when !wide and
/// SQLWCHAR buffers when wide; `bufLen` / `*textLen` count characters (api_diag.cpp).
SQLRETURN odbc2Error(SQLHENV henv, SQLHDBC hdbc, SQLHSTMT hstmt, void* sqlstate, SQLINTEGER* native, void* message,
                     SQLSMALLINT bufLen, SQLSMALLINT* textLen, bool wide);

}  // namespace impl
}  // namespace trino_odbc
