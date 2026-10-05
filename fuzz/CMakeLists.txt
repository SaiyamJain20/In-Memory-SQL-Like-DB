# libFuzzer harnesses. Build with the `fuzz` preset (clang): cmake --preset fuzz && cmake --build --preset fuzz
# Run:  tools/run_fuzz.sh parser 60
add_executable(parser_fuzz parser_fuzz.cpp)
target_link_libraries(parser_fuzz PRIVATE cdb)
target_link_options(parser_fuzz PRIVATE -fsanitize=fuzzer)
