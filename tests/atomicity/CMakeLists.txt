# neko.atomicity — atomic patching regression (Linux only).
add_executable(atomic_runner runner.cpp hot.cpp)
target_link_libraries(atomic_runner PRIVATE nekomata::backends::elf)
set_target_properties(atomic_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(atomic_runner PRIVATE -O0 -fno-pie -fno-pic)
target_link_options(atomic_runner PRIVATE -no-pie)
set_source_files_properties(hot.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
nekomata_apply_warnings(atomic_runner)

configure_file(atomicity.sh.in "${CMAKE_CURRENT_BINARY_DIR}/atomicity-run.sh" @ONLY)

add_test(NAME neko.atomicity
  COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/atomicity-run.sh")
set_tests_properties(neko.atomicity PROPERTIES TIMEOUT 180)
