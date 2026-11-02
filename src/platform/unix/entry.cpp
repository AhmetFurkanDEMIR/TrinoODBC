// Unix (Linux / macOS) shared-library entry glue.
//
// The driver manager dlopen()s the driver and immediately starts calling SQL* entry points; there is
// no DllMain equivalent, so a constructor function is the only place that runs before the first
// API call. It applies TRINO_ODBC_LOG_LEVEL / TRINO_ODBC_LOG_PATH so that even the very first
// SQLAllocHandle is traced when a user asked for logging through the environment. Nothing else
// belongs here: curl / handle-registry initialisation is lazy (std::call_once) inside the core.
#include "platform.h"

#include "util/log.h"

#if defined(TRINO_ODBC_UNIX)

namespace trino_odbc {
namespace {

// The driver is linked with -fvisibility=hidden and an explicit export list; the constructor is
// found through the ELF/Mach-O init section, not by name, so it stays hidden.
__attribute__((constructor)) void trinoOdbcLibraryInit() {
  // configureFromEnvironment() is idempotent (first call wins) and never throws: a failure to open
  // the log sink simply disables logging. A throwing constructor would abort the loading process.
  Logger::global().configureFromEnvironment();
}

}  // namespace
}  // namespace trino_odbc

#endif  // TRINO_ODBC_UNIX
