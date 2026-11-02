#pragma once
// Windows setup / prompting glue (ODBC installer entry points live in setup.cpp, DllMain in
// dllmain.cpp). Only meaningful on Windows; the header is empty elsewhere so that
// src/odbc/api_connect.cpp can include it under #ifdef TRINO_ODBC_WINDOWS without extra guards.
#include "platform.h"

#if defined(TRINO_ODBC_WINDOWS)

#include "core/config.h"

namespace trino_odbc {

/// HINSTANCE of the driver DLL (recorded by DllMain; falls back to a lookup by address when the
/// code is linked statically, e.g. into a test executable). Needed to load dialog resources.
HINSTANCE driverModuleHandle();

/// Shows the connection dialog (same template as the DSN setup dialog, minus the DSN fields)
/// pre-filled from `config` and, when the user presses OK, applies the edited values back into
/// `config` (the caller runs finalize()/validate() afterwards). Returns false when the user
/// cancelled; throws DriverError(IM008) when the dialog could not be created.
/// Used by SQLDriverConnect for SQL_DRIVER_PROMPT / SQL_DRIVER_COMPLETE / SQL_DRIVER_COMPLETE_REQUIRED.
bool showConnectDialog(HWND parent, ConnectionConfig& config);

}  // namespace trino_odbc

#endif  // TRINO_ODBC_WINDOWS
