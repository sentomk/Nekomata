# neko.multi_tu — one atomic transaction spanning two object files (Linux only).
add_executable(multi_tu_runner runner.cpp a.cpp b.cpp)
target_link_libraries(multi_tu_runner PRIVATE nekomata::backends::elf)
set_target_properties(multi_tu_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(multi_tu_runner PRIVATE -O0 -fno-pie -fno-pic)
target_link_options(multi_tu_runner PRIVATE -no-pie)
set_source_files_properties(a.cpp b.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
nekomata_apply_warnings(multi_tu_runner)

configure_file(multi_tu.sh.in
  "${CMAKE_CURRENT_BINARY_DIR}/multi-tu-run.sh" @ONLY)

add_test(NAME neko.multi_tu
  COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/multi-tu-run.sh")
set_tests_properties(neko.multi_tu PROPERTIES TIMEOUT 180)
