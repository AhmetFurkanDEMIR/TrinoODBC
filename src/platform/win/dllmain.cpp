// Windows DLL entry point.
//
// Responsibilities are deliberately minimal (everything here runs under the loader lock):
//  * remember the module handle for the setup dialog resources
//  * DisableThreadLibraryCalls: the driver has no per-thread state that needs attach/detach
//  * apply TRINO_ODBC_LOG_LEVEL / TRINO_ODBC_LOG_PATH so the very first API call is traced
// Nothing is torn down on DLL_PROCESS_DETACH: curl_global_cleanup() and joining prefetch threads
// under the loader lock would deadlock or crash during process exit, and the OS reclaims
// everything anyway. Live connections are the application's responsibility (SQLDisconnect).
#include "platform.h"

#if defined(TRINO_ODBC_WINDOWS)

#include "platform/win/setup.h"
#include "util/log.h"

namespace {
HINSTANCE g_driverModule = nullptr;
}  // namespace

namespace trino_odbc {

HINSTANCE driverModuleHandle() {
  if (g_driverModule) return g_driverModule;
  // Static link (unit tests) or a call before DllMain ran: resolve the module containing this
  // function instead of the executable's handle.
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&driverModuleHandle), &module)) {
    return module;
  }
  return GetModuleHandleW(nullptr);
}

}  // namespace trino_odbc

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      g_driverModule = instance;
      DisableThreadLibraryCalls(instance);
      // Idempotent and non-throwing (a failing log sink only disables logging).
      trino_odbc::Logger::global().configureFromEnvironment();
      break;
    case DLL_PROCESS_DETACH:
      // Intentionally no cleanup (see file comment).
      break;
    default:
      break;
  }
  return TRUE;
}

#endif  // TRINO_ODBC_WINDOWS
