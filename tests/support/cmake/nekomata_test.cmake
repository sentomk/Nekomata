# Shared suite registration helpers.
#
# They own the boilerplate every suite repeats: the doctest include path,
# warnings, sanitizers, add_test naming, timeout, and the level label taken
# from the test name's first component (neko.<level>.<module>.<case>).
#
# What deliberately stays explicit in each suite: exact hot-TU compile
# flags, DWARF versions, function-section toggles, sanitizer-specific
# arguments, and any other compile option that is itself a test input.

set(neko_doctest_include "${PROJECT_SOURCE_DIR}/tests/support")

function(nekomata_add_unit_test)
  set(options COMMAND_EXPAND_LISTS)
  set(oneValueArgs NAME TIMEOUT)
  set(multiValueArgs SOURCES LIBRARIES PRIVATE_INCLUDES DEFINES LABELS TEST_ARGS)
  cmake_parse_arguments(PARSE_ARGV 0 neko "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(NOT neko_NAME OR NOT neko_SOURCES)
    message(FATAL_ERROR "nekomata_add_unit_test: NAME and SOURCES are required")
  endif()

  string(REPLACE "." "_" neko_target "${neko_NAME}")
  add_executable(neko_${neko_target} ${neko_SOURCES})
  target_include_directories(neko_${neko_target} PRIVATE
    "${neko_doctest_include}" ${neko_PRIVATE_INCLUDES})
  if(neko_LIBRARIES)
    target_link_libraries(neko_${neko_target} PRIVATE ${neko_LIBRARIES})
  endif()
  if(neko_DEFINES)
    target_compile_definitions(neko_${neko_target} PRIVATE ${neko_DEFINES})
  endif()
  nekomata_apply_warnings(neko_${neko_target})
  nekomata_apply_sanitizers(neko_${neko_target})

  if(NOT neko_TIMEOUT)
    set(neko_TIMEOUT 30)
  endif()
  # The level label rides along automatically: neko.<level>.<module>.<case>.
  string(FIND "${neko_NAME}" "." neko_first_dot)
  string(SUBSTRING "${neko_NAME}" 0 ${neko_first_dot} neko_level)
  if(neko_COMMAND_EXPAND_LISTS)
    add_test(NAME neko.${neko_NAME} COMMAND neko_${neko_target} ${neko_TEST_ARGS}
      COMMAND_EXPAND_LISTS)
  else()
    add_test(NAME neko.${neko_NAME} COMMAND neko_${neko_target} ${neko_TEST_ARGS})
  endif()
  set_tests_properties(neko.${neko_NAME} PROPERTIES TIMEOUT ${neko_TIMEOUT}
    LABELS "${neko_level};${neko_LABELS}")
endfunction()

function(nekomata_add_e2e_test)
  set(options DEBUG_INFO)
  set(oneValueArgs NAME RUNNER SCRIPT TIMEOUT)
  set(multiValueArgs SOURCES HOT_SOURCES LIBRARIES PRIVATE_INCLUDES LABELS TEST_ARGS)
  cmake_parse_arguments(PARSE_ARGV 0 neko "${options}" "${oneValueArgs}" "${multiValueArgs}")
  if(NOT neko_NAME OR NOT neko_RUNNER OR NOT neko_SCRIPT OR NOT neko_SOURCES)
    message(FATAL_ERROR "nekomata_add_e2e_test: NAME, RUNNER, SCRIPT, and SOURCES are required")
  endif()

  add_executable(${neko_RUNNER} ${neko_SOURCES} ${neko_HOT_SOURCES})
  if(NOT neko_LIBRARIES)
    set(neko_LIBRARIES nekomata::backends::elf)
  endif()
  target_link_libraries(${neko_RUNNER} PRIVATE ${neko_LIBRARIES})
  target_include_directories(${neko_RUNNER} PRIVATE
    "${neko_doctest_include}" ${neko_PRIVATE_INCLUDES})
  # The reloadable binary contract: ET_EXEC-shaped, unoptimized, minimal hot
  # objects. These flags are test inputs; suites needing more (debug info)
  # opt in via DEBUG_INFO, everything stricter stays in their own files.
  set_target_properties(${neko_RUNNER} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  set(neko_runner_flags -O0 -fno-pie -fno-pic)
  if(neko_DEBUG_INFO)
    list(APPEND neko_runner_flags -g)
  endif()
  target_compile_options(${neko_RUNNER} PRIVATE ${neko_runner_flags})
  target_link_options(${neko_RUNNER} PRIVATE -no-pie)
  if(neko_HOT_SOURCES)
    set_source_files_properties(${neko_HOT_SOURCES} PROPERTIES
      COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  endif()
  nekomata_apply_warnings(${neko_RUNNER})

  configure_file("${neko_SCRIPT}" "${CMAKE_CURRENT_BINARY_DIR}/${neko_RUNNER}-run.sh" @ONLY)
  add_test(NAME neko.${neko_NAME}
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/${neko_RUNNER}-run.sh" ${neko_TEST_ARGS})
  if(NOT neko_TIMEOUT)
    set(neko_TIMEOUT 180)
  endif()
  set_tests_properties(neko.${neko_NAME} PROPERTIES TIMEOUT ${neko_TIMEOUT}
    LABELS "e2e;${neko_LABELS}")
endfunction()
