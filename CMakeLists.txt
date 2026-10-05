cmake_minimum_required(VERSION 3.21)
project(cdb VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CDB_WERROR "Treat compiler warnings as errors" OFF)
option(CDB_BUILD_TESTS "Build unit tests" ON)
option(CDB_BUILD_BENCH "Build micro-benchmarks" OFF)
option(CDB_BUILD_TOOLS "Build the shell and tools" ON)
set(CDB_SANITIZER "" CACHE STRING "Sanitizer to enable: address (ASan+UBSan) or thread (TSan)")

# ---------------------------------------------------------------------------
# Common compile options, applied to every cdb target through cdb_options.
# Deliberately no -march=native: binaries must run anywhere; hot kernels pick
# their instruction set at runtime (see docs/ARCHITECTURE.md, Phase 5).
# ---------------------------------------------------------------------------
add_library(cdb_options INTERFACE)
target_compile_options(cdb_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
  -fno-omit-frame-pointer)
if(CDB_WERROR)
  target_compile_options(cdb_options INTERFACE -Werror)
endif()

# CDB_ASSERT is active in Debug and in every sanitizer build (RelWithDebInfo defines
# NDEBUG, so we cannot key off NDEBUG alone).
target_compile_definitions(cdb_options INTERFACE
  $<$<CONFIG:Debug>:CDB_ENABLE_ASSERTS=1>)
if(NOT CDB_SANITIZER STREQUAL "")
  target_compile_definitions(cdb_options INTERFACE CDB_ENABLE_ASSERTS=1)
endif()

if(CDB_SANITIZER STREQUAL "address")
  target_compile_options(cdb_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -g)
  target_link_options(cdb_options INTERFACE -fsanitize=address,undefined)
elseif(CDB_SANITIZER STREQUAL "thread")
  target_compile_options(cdb_options INTERFACE -fsanitize=thread -g)
  target_link_options(cdb_options INTERFACE -fsanitize=thread)
elseif(NOT CDB_SANITIZER STREQUAL "")
  message(FATAL_ERROR "CDB_SANITIZER must be empty, 'address' or 'thread'")
endif()

# ---------------------------------------------------------------------------
# Engine library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CDB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_library(cdb ${CDB_SOURCES})
target_include_directories(cdb PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(cdb PUBLIC cdb_options)
target_compile_definitions(cdb PUBLIC CDB_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Tools, tests, benchmarks
# ---------------------------------------------------------------------------
if(CDB_BUILD_TOOLS AND EXISTS ${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()

if(CDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CDB_BUILD_BENCH AND EXISTS ${CMAKE_SOURCE_DIR}/bench/CMakeLists.txt)
  add_subdirectory(bench)
endif()
