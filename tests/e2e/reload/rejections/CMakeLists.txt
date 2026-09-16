# Boundary/error-path tests (Linux only; they exercise the ELF backend).
# Every case must be rejected with a specific message while the process
# keeps running the old code: the trust story, asserted. The cases file is
# a contract: its expected patterns are part of the suite's value.
if(NOT NEKOMATA_TARGET_LINUX)
  return()
endif()

# -g is what the offline manifest is generated from. Nothing else is
# special: the inspector reads what the compiler emits by default, including
# DWARF 5 and the standard library's always-inlined functions. Debug sections
# carry no code, so every address stays as it was.
nekomata_add_e2e_test(
  NAME e2e.reload.rejections
  RUNNER reject_runner
  SOURCES runner.cpp collide.cpp
  HOT_SOURCES hot.cpp
  SCRIPT cases.sh.in
  DEBUG_INFO
  TIMEOUT 300
  LABELS linux
  TEST_ARGS --cli "$<TARGET_FILE:nekomata>")

# The same harness built PIE: PIE hosts work end to end — startup and a
# live reload, with hot objects compiled -fpie to match the host link.
add_executable(reject_runner_pie runner.cpp hot.cpp collide.cpp)
target_link_libraries(reject_runner_pie PRIVATE nekomata::backends::elf)
target_compile_options(reject_runner_pie PRIVATE -O0)
set_source_files_properties(hot.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
nekomata_apply_warnings(reject_runner_pie)
