# neko.rejections — boundary/error-path tests (Linux only; they exercise
# the ELF backend). Every case must be rejected with a specific message
# while the process keeps running the old code: the trust story, asserted.
if(NEKOMATA_TARGET_LINUX)

  add_executable(reject_runner runner.cpp hot.cpp)
  target_link_libraries(reject_runner PRIVATE nekomata::backends::elf)
  set_target_properties(reject_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(reject_runner PRIVATE -O0 -fno-pie -fno-pic)
  target_link_options(reject_runner PRIVATE -no-pie)
  set_source_files_properties(hot.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(reject_runner)

  # The same harness built PIE: process_symbols must refuse it at startup.
  add_executable(reject_runner_pie runner.cpp hot.cpp)
  target_link_libraries(reject_runner_pie PRIVATE nekomata::backends::elf)
  set_source_files_properties(hot.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(reject_runner_pie)

  configure_file(cases.sh.in
    "${CMAKE_CURRENT_BINARY_DIR}/rejections-cases.sh" @ONLY)

  add_test(NAME neko.rejections
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/rejections-cases.sh")
  set_tests_properties(neko.rejections PROPERTIES TIMEOUT 300)

endif()
