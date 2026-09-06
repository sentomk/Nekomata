# Fault injection tests exercise the actual acceptance scripts with mock
# processes, independently of the Linux loader and sanitizer availability.
function(configure_mock template source_dir binary_dir output)
  set(CMAKE_CURRENT_SOURCE_DIR "${source_dir}")
  set(CMAKE_CURRENT_BINARY_DIR "${binary_dir}")
  set(CMAKE_CXX_COMPILER "${binary_dir}/mock_compile.sh")
  configure_file("${template}" "${binary_dir}/${output}" @ONLY)
endfunction()

foreach(mode IN ITEMS success nonzero missing sanitizer)
  add_test(NAME neko.harness.output.${mode}
    COMMAND "${CMAKE_COMMAND}" "-DMODE=${mode}"
      "-DCHECK=${PROJECT_SOURCE_DIR}/tests/check_output.cmake"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/output_regression.cmake")
  set_tests_properties(neko.harness.output.${mode} PROPERTIES TIMEOUT 15)
endforeach()

if(UNIX)
  find_program(NEKOMATA_TEST_BASH bash REQUIRED)
  set(mock_dir "${CMAKE_CURRENT_BINARY_DIR}/mock")
  configure_mock("${PROJECT_SOURCE_DIR}/examples/hello_reload/run_demo.sh.in"
    "${PROJECT_SOURCE_DIR}/examples/hello_reload" "${mock_dir}" run_demo.sh)
  configure_mock("${PROJECT_SOURCE_DIR}/tests/rejections/cases.sh.in"
    "${PROJECT_SOURCE_DIR}/tests/rejections" "${mock_dir}" cases.sh)
  foreach(name IN ITEMS hello_reload reject_runner reject_runner_pie)
    configure_file(mock_runner.sh "${mock_dir}/${name}" COPYONLY)
    file(CHMOD "${mock_dir}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  endforeach()
  foreach(name IN ITEMS mock_compile.sh rebuild_hot.sh)
    configure_file(mock_compile.sh "${mock_dir}/${name}" COPYONLY)
    file(CHMOD "${mock_dir}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  endforeach()

  function(add_harness_case name script expected)
    add_test(NAME neko.harness.${name}
      COMMAND "${CMAKE_COMMAND}" "-DMODE=${name}" "-DEXPECTED=${expected}"
        "-DBASH=${NEKOMATA_TEST_BASH}" "-DSCRIPT=${mock_dir}/${script}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/${name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/script_regression.cmake")
    set_tests_properties(neko.harness.${name} PROPERTIES TIMEOUT 45)
  endfunction()

  add_harness_case(hello_success run_demo.sh "")
  add_harness_case(hello_nonzero run_demo.sh "runner exited with status 42")
  add_harness_case(hello_shutdown_nonzero run_demo.sh "runner exited with status 42")
  add_harness_case(hello_signal run_demo.sh "runner exited with status")
  add_harness_case(hello_early_zero run_demo.sh "before acknowledging shutdown")
  add_harness_case(hello_sanitizer run_demo.sh "sanitizer diagnostic")
  add_harness_case(reject_success cases.sh "")
  add_harness_case(reject_stale_truncated cases.sh "FAIL(truncated)")
  add_harness_case(reject_stale_final cases.sh "expected new 'reload applied'")
  add_harness_case(reject_ignored_final cases.sh "expected new '\\[recovered\\] tick'")
  add_harness_case(reject_state_reset cases.sh "new logic or state continuity")
  add_harness_case(reject_nonzero cases.sh "runner exited with status 42")
  add_harness_case(pie_zero cases.sh "expected exit 1, got 0")
  add_harness_case(pie_nonzero cases.sh "expected exit 1, got 42")
endif()
