include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE CDB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(cdb_tests ${CDB_TEST_SOURCES})
target_include_directories(cdb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cdb_tests PRIVATE cdb GTest::gtest)

# ThreadSanitizer's runtime aborts ("unexpected memory mapping") on kernels with high mmap ASLR
# entropy (vm.mmap_rnd_bits > 28, the default on recent Ubuntu/Linux). Running the test binary
# with ASLR disabled for that process works around it without needing root. Harmless elsewhere.
if(CDB_SANITIZER STREQUAL "thread")
  find_program(CDB_SETARCH setarch)
  if(CDB_SETARCH)
    set_property(TARGET cdb_tests PROPERTY CROSSCOMPILING_EMULATOR
      ${CDB_SETARCH} ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
  endif()
endif()

include(GoogleTest)
gtest_discover_tests(cdb_tests DISCOVERY_TIMEOUT 60)
