# libFuzzer harnesses. Build with the `fuzz` preset (clang): cmake --preset fuzz && cmake --build --preset fuzz
# Run:  tools/run_fuzz.sh parser 60     (also: checkpoint, wal)
foreach(target parser checkpoint wal)
  add_executable(${target}_fuzz ${target}_fuzz.cpp)
  target_link_libraries(${target}_fuzz PRIVATE cdb)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer)
endforeach()
