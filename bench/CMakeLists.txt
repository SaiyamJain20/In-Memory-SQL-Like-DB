include(FetchContent)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

file(GLOB CDB_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(cdb_bench ${CDB_BENCH_SOURCES})
target_include_directories(cdb_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(cdb_bench PRIVATE cdb benchmark::benchmark_main)

# TPC-H timing runner (not a Google Benchmark): loads generated data and times whole queries.
add_executable(cdb_tpch tpch/tpch_runner.cpp)
target_compile_definitions(cdb_tpch PRIVATE CDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(cdb_tpch PRIVATE cdb)
