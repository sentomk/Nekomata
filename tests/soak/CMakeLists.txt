# neko.soak — multi-generation reload soak (Linux only). The driver script
# drops randomized logic variants into one live process; the runner proves
# per-generation state continuity and reports allocation accounting from
# its own memory map (r-xp region + PROT_NONE guard = one arena).
add_executable(soak_runner runner.cpp hot.cpp)
target_link_libraries(soak_runner PRIVATE nekomata::backends::elf)
set_target_properties(soak_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(soak_runner PRIVATE -O0 -fno-pie -fno-pic)
target_link_options(soak_runner PRIVATE -no-pie)
set_source_files_properties(hot.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
nekomata_apply_warnings(soak_runner)

configure_file(soak.sh.in "${CMAKE_CURRENT_BINARY_DIR}/soak-run.sh" @ONLY)

add_test(NAME neko.soak
  COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/soak-run.sh")
set_tests_properties(neko.soak PROPERTIES TIMEOUT 180)
