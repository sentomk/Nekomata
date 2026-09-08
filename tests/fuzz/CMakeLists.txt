# libFuzzer targets (Clang only; the relocations- and symbol-heavy ELF
# parsing surface is pure and memory-based, ideal for coverage-guided
# fuzzing). The PR burst replays the committed corpus (corpus/) and then
# explores for a few minutes; sanitizer reports and crashes are the bugs.
add_executable(fuzz_parse_object fuzz_parse_object.cpp)
target_include_directories(fuzz_parse_object PRIVATE
  "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(fuzz_parse_object PRIVATE neko_backend_elf)
target_compile_options(fuzz_parse_object PRIVATE
  -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined)
target_link_options(fuzz_parse_object PRIVATE
  -fsanitize=fuzzer,address,undefined)
