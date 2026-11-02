cmake_minimum_required(VERSION 3.16)
project(trinoodbc VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------------
set(TRINO_ODBC_DRIVER_MANAGER "auto" CACHE STRING "auto | unixodbc | iodbc | windows")
set_property(CACHE TRINO_ODBC_DRIVER_MANAGER PROPERTY STRINGS auto unixodbc iodbc windows)
set(TRINO_ODBC_WITH_ZSTD "auto" CACHE STRING "auto | ON | OFF  (json+zstd spooled encoding)")
set(TRINO_ODBC_WITH_LZ4 "auto" CACHE STRING "auto | ON | OFF  (json+lz4 spooled encoding)")
set(TRINO_ODBC_USE_SYSTEM_SIMDJSON "auto" CACHE STRING "auto | ON | OFF (OFF = FetchContent)")
option(TRINO_ODBC_BUILD_TESTS "Build unit and integration tests" ON)
option(TRINO_ODBC_WERROR "Treat warnings as errors" OFF)
option(TRINO_ODBC_INSTALL_ODBCINST "Register the driver with odbcinst on install (Unix)" OFF)
set(TRINO_ODBC_SIMDJSON_VERSION "v3.13.0" CACHE STRING "simdjson tag for FetchContent")

include(TrinoOdbcHelpers)

# ---------------------------------------------------------------------------------------------
# Driver manager (ODBC headers + odbcinst)
# ---------------------------------------------------------------------------------------------
trino_odbc_find_driver_manager()   # sets TRINO_ODBC_DM, ODBC_INCLUDE_DIRS, ODBC_INST_LIBRARIES, ODBC_LIBRARIES
# Record the driver manager the library is compiled for (SQLWCHAR width and installer library are
# fixed at build time): packaging reads it so a build is only ever registered with its own DM.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DRIVER_MANAGER" "${TRINO_ODBC_DM}\n")

# ---------------------------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------------------------
find_package(CURL REQUIRED)
trino_odbc_find_simdjson()         # target simdjson::simdjson
trino_odbc_find_optional_codecs()  # TRINO_ODBC_HAVE_ZSTD / TRINO_ODBC_HAVE_LZ4 + targets
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------------------------
set(TRINO_ODBC_CORE_SOURCES
  src/util/log.cpp
  src/util/strings.cpp
  src/util/unicode.cpp
  src/util/tz.cpp
  src/core/diag.cpp
  src/core/config.cpp
  src/core/platform_setup.cpp
  src/core/handles.cpp
  src/types/trino_type.cpp
  src/types/odbc_types.cpp
  src/client/http_client.cpp
  src/client/trino_client.cpp
  src/client/oauth2.cpp
  src/client/spool_decoder.cpp
  src/result/page.cpp
  src/result/json_decoder.cpp
  src/result/row_source.cpp
  src/result/result_set.cpp
  src/convert/convert.cpp
  src/convert/temporal.cpp
  src/convert/numeric.cpp
  src/convert/params.cpp
  src/sql/escape.cpp
  src/catalog/catalog.cpp
  src/catalog/type_info.cpp
  src/odbc/api_common.cpp
  src/odbc/api_handles.cpp
  src/odbc/api_connect.cpp
  src/odbc/api_attrs.cpp
  src/odbc/api_info.cpp
  src/odbc/api_statement.cpp
  src/odbc/api_results.cpp
  src/odbc/api_catalog.cpp
  src/odbc/api_diag.cpp
  src/odbc/api_desc.cpp
  src/odbc/api_unicode.cpp
)

set(TRINO_ODBC_PLATFORM_SOURCES)
if(WIN32)
  list(APPEND TRINO_ODBC_PLATFORM_SOURCES
    src/platform/win/dllmain.cpp
    src/platform/win/setup.cpp
    src/platform/win/trinoodbc.rc
    src/platform/win/trinoodbc.def)
  # trinoodbc.rc references resource.h and the Trino logo icon (trinoodbc.ico) by bare file name:
  # give rc.exe / windres the .rc directory as include path and rebuild the resources when the
  # icon changes.
  set_source_files_properties(src/platform/win/trinoodbc.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win/trinoodbc.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win/resource.h")
else()
  list(APPEND TRINO_ODBC_PLATFORM_SOURCES src/platform/unix/entry.cpp)
endif()

# Static core used by the shared driver and by the tests.
add_library(trinoodbc_core STATIC ${TRINO_ODBC_CORE_SOURCES})
target_include_directories(trinoodbc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${ODBC_INCLUDE_DIRS})
target_link_libraries(trinoodbc_core PUBLIC CURL::libcurl simdjson::simdjson Threads::Threads ${ODBC_INST_LIBRARIES})
target_compile_definitions(trinoodbc_core PUBLIC
  TRINO_ODBC_VERSION_STRING="${PROJECT_VERSION}"
  $<$<BOOL:${TRINO_ODBC_HAVE_ZSTD}>:TRINO_ODBC_HAVE_ZSTD=1>
  $<$<BOOL:${TRINO_ODBC_HAVE_LZ4}>:TRINO_ODBC_HAVE_LZ4=1>
  ${TRINO_ODBC_DM_DEFINES})
if(TRINO_ODBC_HAVE_ZSTD)
  target_link_libraries(trinoodbc_core PUBLIC ${TRINO_ODBC_ZSTD_TARGET})
endif()
if(TRINO_ODBC_HAVE_LZ4)
  target_link_libraries(trinoodbc_core PUBLIC ${TRINO_ODBC_LZ4_TARGET})
endif()
if(WIN32)
  target_link_libraries(trinoodbc_core PUBLIC ws2_32 crypt32 wldap32 normaliz secur32 bcrypt)
elseif(APPLE)
  target_link_libraries(trinoodbc_core PUBLIC "-framework CoreFoundation")
else()
  target_link_libraries(trinoodbc_core PUBLIC ${CMAKE_DL_LIBS})
endif()
trino_odbc_set_warnings(trinoodbc_core)

# The driver shared library (exported SQL* entry points live in the core; the shared library only adds
# platform glue so that the same object code is tested and shipped).
add_library(trinoodbc SHARED ${TRINO_ODBC_PLATFORM_SOURCES})
target_link_libraries(trinoodbc PRIVATE trinoodbc_core)
if(WIN32)
  set(TRINO_ODBC_LIB_PREFIX "")
else()
  set(TRINO_ODBC_LIB_PREFIX "lib")
endif()
set_target_properties(trinoodbc PROPERTIES
  OUTPUT_NAME "trinoodbc"
  PREFIX "${TRINO_ODBC_LIB_PREFIX}"
  CXX_VISIBILITY_PRESET hidden)
if(WIN32)
  target_link_libraries(trinoodbc PRIVATE odbccp32 odbc32 comctl32 user32 advapi32)
  if(MSVC)
    # MSVC-only import library (old stdio symbols for pre-2015 static libraries); mingw-w64 has none.
    target_link_libraries(trinoodbc PRIVATE legacy_stdio_definitions)
  endif()
  set_target_properties(trinoodbc PROPERTIES SUFFIX ".dll")
elseif(APPLE)
  set_target_properties(trinoodbc PROPERTIES SUFFIX ".dylib" MACOSX_RPATH ON INSTALL_NAME_DIR "@rpath")
  target_link_options(trinoodbc PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/platform/unix/exports.macos")
else()
  set_target_properties(trinoodbc PROPERTIES SUFFIX ".so")
  # -Bsymbolic-functions binds the driver's calls to its own SQL* functions inside the library: the
  # driver manager exports the same names, and ELF interposition would otherwise route e.g. an
  # internal SQLFreeHandle into the DM (the version script only hides symbols, it does not bind them).
  target_link_options(trinoodbc PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/unix/exports.map"
    "-Wl,-Bsymbolic-functions"
    "-Wl,--no-undefined")
endif()
# Force the static core's exported functions to be kept in the shared library.
trino_odbc_whole_archive(trinoodbc trinoodbc_core)
trino_odbc_set_warnings(trinoodbc)

# Benchmark utility (loads the driver directly).
add_executable(trino_odbc_bench tools/trino_odbc_bench.cpp)
target_include_directories(trino_odbc_bench PRIVATE ${ODBC_INCLUDE_DIRS})
target_link_libraries(trino_odbc_bench PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(trino_odbc_bench trinoodbc)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
if(TRINO_ODBC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS trinoodbc
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/platform/unix/odbcinst.ini.in DESTINATION ${CMAKE_INSTALL_DATADIR}/trinoodbc RENAME odbcinst.ini.template OPTIONAL)
if(NOT WIN32)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/DRIVER_MANAGER" DESTINATION ${CMAKE_INSTALL_DATADIR}/trinoodbc)
endif()
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
install(FILES README.md THIRD-PARTY-NOTICES.md LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
if(NOT WIN32 AND TRINO_ODBC_INSTALL_ODBCINST)
  # Register only with the driver manager this build targets: register_driver.sh prefers unixODBC's
  # odbcinst(1), which must never receive an iODBC build (4-byte SQLWCHAR, iODBC installer library).
  set(_trino_odbc_register_args "")
  if(TRINO_ODBC_DM STREQUAL "iodbc")
    if(APPLE)
      set(_trino_odbc_register_args "--no-odbcinst --odbcinst /Library/ODBC/odbcinst.ini")
    else()
      set(_trino_odbc_register_args "--no-odbcinst")
    endif()
  endif()
  install(CODE "execute_process(COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/register_driver.sh ${_trino_odbc_register_args} \"${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:trinoodbc>\")")
endif()

set(CPACK_PACKAGE_NAME "trino-odbc")
set(CPACK_PACKAGE_VENDOR "Trino ODBC")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/AhmetFurkanDEMIR/TrinoODBC")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ODBC 3.8 driver for Trino")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "trino-odbc")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "unixodbc, libcurl4")
set(CPACK_RPM_PACKAGE_REQUIRES "unixODBC, libcurl")
set(CPACK_GENERATOR "TGZ")
include(CPack)

message(STATUS "Trino ODBC: driver manager=${TRINO_ODBC_DM} zstd=${TRINO_ODBC_HAVE_ZSTD} lz4=${TRINO_ODBC_HAVE_LZ4} simdjson=${TRINO_ODBC_SIMDJSON_SOURCE}")
