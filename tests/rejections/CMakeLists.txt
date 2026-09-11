# neko.rejections — boundary/error-path tests (Linux only; they exercise
# the ELF backend). Every case must be rejected with a specific message
# while the process keeps running the old code: the trust story, asserted.
if(NEKOMATA_TARGET_LINUX)

  add_executable(reject_runner runner.cpp hot.cpp collide.cpp)
  target_link_libraries(reject_runner PRIVATE nekomata::backends::elf)
  set_target_properties(reject_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  # -g is what the manifest is generated from, and -gdwarf-4 because the
  # inspector reads DWARF 4: GCC and Clang emit DWARF 5 by default now, which
  # it refuses rather than misreads. Debug sections carry no code, so the
  # reload behaviour and every address stay as they were.
  target_compile_options(reject_runner PRIVATE
    -O0 -g -gdwarf-4 -fno-inline -fno-pie -fno-pic)
  target_link_options(reject_runner PRIVATE -no-pie)
  set_source_files_properties(hot.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(reject_runner)

  # The same harness built PIE: process_symbols must refuse it at startup.
  add_executable(reject_runner_pie runner.cpp hot.cpp collide.cpp)
  target_link_libraries(reject_runner_pie PRIVATE nekomata::backends::elf)
  target_compile_options(reject_runner_pie PRIVATE -O0)
  set_source_files_properties(hot.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(reject_runner_pie)

  configure_file(cases.sh.in
    "${CMAKE_CURRENT_BINARY_DIR}/rejections-cases.sh" @ONLY)

  add_test(NAME neko.rejections
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/rejections-cases.sh"
      --cli "$<TARGET_FILE:nekomata>")
  set_tests_properties(neko.rejections PROPERTIES TIMEOUT 300)

endif()
