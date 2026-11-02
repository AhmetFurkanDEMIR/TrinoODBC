# Unit tests link the static core; integration tests dlopen the shared driver.
add_executable(trino_odbc_unit_tests
  unit/test_main.cpp
  unit/test_strings_unicode.cpp
  unit/test_config.cpp
  unit/test_types.cpp
  unit/test_escape.cpp
  unit/test_page_result.cpp
  unit/test_convert.cpp
  unit/test_params.cpp
  unit/test_temporal.cpp
  unit/test_numeric.cpp
  unit/test_json_decoder.cpp
  unit/test_spool_decoder.cpp
  unit/test_diag.cpp
  unit/test_catalog_sql.cpp
)
target_link_libraries(trino_odbc_unit_tests PRIVATE trinoodbc_core)
target_include_directories(trino_odbc_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit COMMAND trino_odbc_unit_tests)

set(TRINO_ODBC_INTEGRATION_SOURCES
  integration/main.cpp
  integration/odbc_loader.cpp
  integration/test_connect.cpp
  integration/test_metadata.cpp
  integration/test_query.cpp
  integration/test_fetch.cpp
  integration/test_params.cpp
  integration/test_catalog.cpp
  integration/test_errors.cpp
  integration/test_powerbi_flow.cpp)
set(TRINO_ODBC_INTEGRATION_COMPLETE TRUE)
foreach(src ${TRINO_ODBC_INTEGRATION_SOURCES})
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    set(TRINO_ODBC_INTEGRATION_COMPLETE FALSE)
    message(STATUS "integration tests skipped: missing ${src}")
  endif()
endforeach()
if(TRINO_ODBC_INTEGRATION_COMPLETE)
add_executable(trino_odbc_integration_tests ${TRINO_ODBC_INTEGRATION_SOURCES})
target_include_directories(trino_odbc_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${ODBC_INCLUDE_DIRS})
target_link_libraries(trino_odbc_integration_tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(trino_odbc_integration_tests PRIVATE
  TRINO_ODBC_DRIVER_PATH="$<TARGET_FILE:trinoodbc>"
  TRINO_ODBC_MOCK_SERVER="${CMAKE_SOURCE_DIR}/tests/mock_trino/mock_trino.py")
if(NOT WIN32)
  target_link_libraries(trino_odbc_integration_tests PRIVATE ${ODBC_INST_LIBRARIES})
endif()
add_dependencies(trino_odbc_integration_tests trinoodbc)
add_test(NAME integration_mock COMMAND trino_odbc_integration_tests --mock)
endif()
