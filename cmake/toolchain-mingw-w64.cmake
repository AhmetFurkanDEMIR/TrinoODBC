# CMake toolchain file: cross-compile for Windows x64 with mingw-w64 (GCC) from macOS or Linux.
#
#   cmake -S . -B build-mingw -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#         -DCMAKE_PREFIX_PATH=/path/to/mingw-prefix \
#         -DTRINO_ODBC_DRIVER_MANAGER=windows -DTRINO_ODBC_BUILD_TESTS=OFF
#
# packaging/windows/cross_build_mingw.sh uses this file both for the static dependency builds
# (zstd, lz4, simdjson, libcurl) and for the driver.
#
# Nothing here is specific to one machine: the compiler is looked up on PATH (or taken from
# TRINO_MINGW_TRIPLE / CMAKE_C_COMPILER) and the mingw-w64 sysroot is derived from the compiler
# itself, so Homebrew (/opt/homebrew/Cellar/mingw-w64/<v>/toolchain-x86_64/x86_64-w64-mingw32) and
# Debian/Ubuntu/Fedora (/usr/x86_64-w64-mingw32) layouts both work.
#
# Options (cache variables, pass with -D):
#   TRINO_MINGW_TRIPLE          compiler prefix (default x86_64-w64-mingw32)
#   TRINO_MINGW_STATIC_RUNTIME  ON (default): link libgcc, libstdc++ and winpthread statically so
#                               the DLL depends on Windows system DLLs only.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Toolchain files are evaluated several times (once per try_compile project); cache the settings so
# every evaluation sees the same values.
set(TRINO_MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "mingw-w64 target triple / tool prefix")
set(TRINO_MINGW_STATIC_RUNTIME ON CACHE BOOL "Link libgcc/libstdc++/winpthread statically")
# try_compile projects receive only the variables listed here.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TRINO_MINGW_TRIPLE TRINO_MINGW_STATIC_RUNTIME TRINO_MINGW_ROOT)

if(NOT CMAKE_C_COMPILER)
  find_program(TRINO_MINGW_GCC NAMES ${TRINO_MINGW_TRIPLE}-gcc ${TRINO_MINGW_TRIPLE}-gcc-posix)
  if(NOT TRINO_MINGW_GCC)
    message(FATAL_ERROR "${TRINO_MINGW_TRIPLE}-gcc not found on PATH "
                        "(macOS: brew install mingw-w64, Debian/Ubuntu: apt install mingw-w64)")
  endif()
  set(CMAKE_C_COMPILER "${TRINO_MINGW_GCC}")
endif()
if(NOT CMAKE_CXX_COMPILER)
  find_program(TRINO_MINGW_GXX NAMES ${TRINO_MINGW_TRIPLE}-g++ ${TRINO_MINGW_TRIPLE}-g++-posix)
  if(NOT TRINO_MINGW_GXX)
    message(FATAL_ERROR "${TRINO_MINGW_TRIPLE}-g++ not found on PATH")
  endif()
  set(CMAKE_CXX_COMPILER "${TRINO_MINGW_GXX}")
endif()
if(NOT CMAKE_RC_COMPILER)
  find_program(TRINO_MINGW_WINDRES NAMES ${TRINO_MINGW_TRIPLE}-windres windres)
  if(NOT TRINO_MINGW_WINDRES)
    message(FATAL_ERROR "${TRINO_MINGW_TRIPLE}-windres not found on PATH")
  endif()
  set(CMAKE_RC_COMPILER "${TRINO_MINGW_WINDRES}")
endif()
foreach(_tool ar ranlib nm objdump strip dlltool)
  string(TOUPPER "${_tool}" _TOOL)
  if(NOT CMAKE_${_TOOL})
    find_program(TRINO_MINGW_${_TOOL} NAMES ${TRINO_MINGW_TRIPLE}-${_tool})
    if(TRINO_MINGW_${_TOOL})
      set(CMAKE_${_TOOL} "${TRINO_MINGW_${_TOOL}}" CACHE FILEPATH "mingw-w64 ${_tool}")
    endif()
  endif()
endforeach()

# Locate the target sysroot from the compiler: the directory holding libkernel32.a is
# <root>/lib, whatever the distribution's layout is. -print-sysroot is empty on most Linux builds of
# mingw-w64, so it cannot be relied upon.
if(NOT TRINO_MINGW_ROOT)
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libkernel32.a
    OUTPUT_VARIABLE _trino_kernel32
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _trino_rc)
  if(NOT _trino_rc EQUAL 0 OR NOT IS_ABSOLUTE "${_trino_kernel32}" OR NOT EXISTS "${_trino_kernel32}")
    message(FATAL_ERROR "Cannot locate the mingw-w64 sysroot: '${CMAKE_C_COMPILER} "
                        "-print-file-name=libkernel32.a' returned '${_trino_kernel32}'")
  endif()
  get_filename_component(_trino_libdir "${_trino_kernel32}" DIRECTORY)
  get_filename_component(_trino_root "${_trino_libdir}/.." ABSOLUTE)
  set(TRINO_MINGW_ROOT "${_trino_root}" CACHE PATH "mingw-w64 target sysroot (contains include/ and lib/)")
endif()

# Search headers, libraries and CMake package files only in the mingw sysroot and in the
# dependency prefixes named by CMAKE_PREFIX_PATH (never in the build host's /opt/homebrew or /usr,
# whose Mach-O / ELF libraries cannot be linked into a PE DLL). Programs run on the host.
set(CMAKE_FIND_ROOT_PATH "${TRINO_MINGW_ROOT}")
foreach(_trino_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(_trino_prefix)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_trino_prefix}")
  endif()
endforeach()
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config (used by some Find modules, e.g. FindCURL's fallback) must only see the .pc files of the
# Windows prefixes, never the host's.
set(_trino_pc_dirs "")
foreach(_trino_root IN LISTS CMAKE_FIND_ROOT_PATH)
  list(APPEND _trino_pc_dirs "${_trino_root}/lib/pkgconfig" "${_trino_root}/share/pkgconfig")
endforeach()
string(REPLACE ";" ":" _trino_pc_dirs "${_trino_pc_dirs}")
set(ENV{PKG_CONFIG_LIBDIR} "${_trino_pc_dirs}")
set(ENV{PKG_CONFIG_PATH} "")

if(TRINO_MINGW_STATIC_RUNTIME)
  # -static: prefer libwinpthread.a over libwinpthread.dll.a (std::thread / std::mutex use it with
  # the posix thread model). The Windows import libraries (odbc32, ws2_32, ...) stay DLL imports.
  set(_trino_static_rt "-static -static-libgcc -static-libstdc++")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_trino_static_rt}")
  set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_trino_static_rt}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT "${_trino_static_rt}")
endif()
