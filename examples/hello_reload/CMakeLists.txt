# hello_reload — the single-translation-unit hot-reload acceptance demo.
#
# hot.cpp is a translation unit with mutable global state (g_counter, plus a
# file-static). run_demo.sh starts this runner, edits hot.cpp's logic, drops
# a fresh object file at the watched path, and asserts:
#   1. the process did not restart, yet later calls run the new logic;
#   2. g_counter / s_calls kept counting across the swap (state preserved).
#
# Build requirements (current pruning): -no-pie (process_symbols reads the
# executable's .symtab and expects link-time == runtime addresses) and -O0
# (entry-prologue pattern). PIE support is planned.

add_executable(hello_reload main.cpp hot.cpp)
target_link_libraries(hello_reload PRIVATE nekomata::backends::elf)

set_target_properties(hello_reload PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(hello_reload PRIVATE -O0 -fno-pie -fno-pic)
# The reloadable TU is built without exceptions/unwind tables so its object
# stays minimal (no .eh_frame, no LSDA); main.cpp keeps full C++.
set_source_files_properties(hot.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
target_link_options(hello_reload PRIVATE -no-pie)

# The "build information" answer: the exact compiler command for rebuilding
# hot.cpp, kept in sync with the flags that linked the original binary.
configure_file(rebuild_hot.sh.in "${CMAKE_CURRENT_BINARY_DIR}/rebuild_hot.sh" @ONLY)
configure_file(run_demo.sh.in "${CMAKE_CURRENT_BINARY_DIR}/run_demo.sh" @ONLY)

if(NEKOMATA_BUILD_TESTS)
  add_test(NAME neko.hello_reload
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/run_demo.sh")
  set_tests_properties(neko.hello_reload PROPERTIES TIMEOUT 300)
endif()
