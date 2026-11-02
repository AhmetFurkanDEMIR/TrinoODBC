#pragma once
// Platform / ODBC header shim. Every translation unit in the driver includes this first.
//
// Responsibilities:
//  * pull in the right ODBC headers for Windows / unixODBC / iODBC
//  * define export macros
//  * expose the SQLWCHAR width (2 = UTF-16 on Windows & unixODBC, 4 = UTF-32 on iODBC)
//  * provide ODBC 3.8 constants that older driver-manager headers (iODBC) lack

#if defined(_WIN32)
#  define TRINO_ODBC_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  define TRINO_ODBC_MACOS 1
#  define TRINO_ODBC_UNIX 1
#else
#  define TRINO_ODBC_LINUX 1
#  define TRINO_ODBC_UNIX 1
#endif

#ifndef ODBCVER
#  define ODBCVER 0x0380
#endif
// The driver defines every ANSI and Unicode entry point itself; never let sqlucode.h remap the
// ANSI names to the W variants when UNICODE is defined (MSVC build).
#ifndef SQL_NOUNICODEMAP
#  define SQL_NOUNICODEMAP 1
#endif

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <sqlucode.h>
#if defined(__has_include)
#  if __has_include(<odbcinst.h>)
#    include <odbcinst.h>
#  elif __has_include(<iodbcinst.h>)
#    include <iodbcinst.h>
#  else
#    error "ODBC installer header (odbcinst.h) not found"
#  endif
#else
#  include <odbcinst.h>
#endif

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define TRINO_ODBC_API __declspec(dllexport)
#else
#  define TRINO_ODBC_API __attribute__((visibility("default")))
#endif

#ifndef SQL_API
#  define SQL_API
#endif

static_assert(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4, "unsupported SQLWCHAR width");

// ---- ODBC 3.8 constants missing from some driver-manager headers ---------------------------
#ifndef SQL_OV_ODBC3_80
#  define SQL_OV_ODBC3_80 380UL
#endif
#ifndef SQL_ATTR_ASYNC_DBC_FUNCTIONS
#  define SQL_ATTR_ASYNC_DBC_FUNCTIONS 117
#endif
#ifndef SQL_ASYNC_DBC_ENABLE_OFF
#  define SQL_ASYNC_DBC_ENABLE_OFF 0UL
#endif
#ifndef SQL_ASYNC_DBC_FUNCTIONS
#  define SQL_ASYNC_DBC_FUNCTIONS 10023
#endif
#ifndef SQL_ASYNC_DBC_NOT_CAPABLE
#  define SQL_ASYNC_DBC_NOT_CAPABLE 0x00000000L
#endif
#ifndef SQL_DRIVER_AWARE_POOLING_SUPPORTED
#  define SQL_DRIVER_AWARE_POOLING_SUPPORTED 10024
#endif
#ifndef SQL_DRIVER_AWARE_POOLING_NOT_CAPABLE
#  define SQL_DRIVER_AWARE_POOLING_NOT_CAPABLE 0x00000000L
#endif
#ifndef SQL_ASYNC_NOTIFICATION
#  define SQL_ASYNC_NOTIFICATION 10025
#endif
#ifndef SQL_ASYNC_NOTIFICATION_NOT_CAPABLE
#  define SQL_ASYNC_NOTIFICATION_NOT_CAPABLE 0x00000000L
#endif
#ifndef SQL_ATTR_RESET_CONNECTION
#  define SQL_ATTR_RESET_CONNECTION 116
#endif
#ifndef SQL_RESET_CONNECTION_YES
#  define SQL_RESET_CONNECTION_YES 1UL
#endif
#ifndef SQL_ATTR_DBC_INFO_TOKEN
#  define SQL_ATTR_DBC_INFO_TOKEN 118
#endif
#ifndef SQL_ATTR_ASYNC_DBC_EVENT
#  define SQL_ATTR_ASYNC_DBC_EVENT 119
#endif
#ifndef SQL_ATTR_ASYNC_STMT_EVENT
#  define SQL_ATTR_ASYNC_STMT_EVENT 29
#endif
#ifndef SQL_ATTR_ANSI_APP
#  define SQL_ATTR_ANSI_APP 115
#endif
#ifndef SQL_AA_TRUE
#  define SQL_AA_TRUE 1L
#endif
#ifndef SQL_AA_FALSE
#  define SQL_AA_FALSE 0L
#endif
#ifndef SQL_CP_DRIVER_AWARE
#  define SQL_CP_DRIVER_AWARE 3UL
#endif
#ifndef SQL_API_SQLCANCELHANDLE
#  define SQL_API_SQLCANCELHANDLE 1550
#endif
#ifndef SQL_API_SQLCOMPLETEASYNC
#  define SQL_API_SQLCOMPLETEASYNC 1551
#endif
#ifndef SQL_PARAM_DATA_AVAILABLE
#  define SQL_PARAM_DATA_AVAILABLE 101
#endif
#ifndef SQL_CONVERT_GUID
#  define SQL_CONVERT_GUID 173
#endif
#ifndef SQL_CVT_GUID
#  define SQL_CVT_GUID 0x01000000L
#endif
#ifndef SQL_NO_ROW_NUMBER
#  define SQL_NO_ROW_NUMBER (-1)
#endif
#ifndef SQL_NO_COLUMN_NUMBER
#  define SQL_NO_COLUMN_NUMBER (-1)
#endif
#ifndef SQL_ROW_NUMBER_UNKNOWN
#  define SQL_ROW_NUMBER_UNKNOWN (-2)
#endif
#ifndef SQL_COLUMN_NUMBER_UNKNOWN
#  define SQL_COLUMN_NUMBER_UNKNOWN (-2)
#endif
#ifndef SQL_ATTR_METADATA_ID
#  define SQL_ATTR_METADATA_ID 10014
#endif
#ifndef SQL_DESC_ARRAY_SIZE
#  define SQL_DESC_ARRAY_SIZE 20
#endif
#ifndef SQL_C_GUID
#  define SQL_C_GUID SQL_GUID
#endif
#ifndef SQL_IS_POINTER
#  define SQL_IS_POINTER (-4)
#endif

namespace trino_odbc {
constexpr bool kWideIsUtf16 = sizeof(SQLWCHAR) == 2;
constexpr const char* kDriverVersion = "1.0.0";
constexpr const char* kDriverVersionOdbc = "01.00.0000";  // SQL_DRIVER_VER format ##.##.####
constexpr const char* kDriverOdbcVersion = "03.80";
constexpr const char* kUserAgent = "TrinoODBC/1.0.0";
#if defined(TRINO_ODBC_WINDOWS)
constexpr const char* kDriverFileName = "trinoodbc.dll";
#elif defined(TRINO_ODBC_MACOS)
constexpr const char* kDriverFileName = "libtrinoodbc.dylib";
#else
constexpr const char* kDriverFileName = "libtrinoodbc.so";
#endif
}  // namespace trino_odbc
