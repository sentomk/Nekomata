# Regression tests for the public API acceptance gate.
foreach(mode IN ITEMS success nonzero missing sanitizer)
  add_test(NAME neko.harness.output.${mode}
    COMMAND "${CMAKE_COMMAND}" "-DMODE=${mode}"
      "-DCHECK=${PROJECT_SOURCE_DIR}/tests/check_output.cmake"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/output_regression.cmake")
  set_tests_properties(neko.harness.output.${mode} PROPERTIES TIMEOUT 15)
endforeach()
