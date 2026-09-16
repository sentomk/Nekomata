find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_test(NAME neko.tooling.envsetup
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/envsetup_test.py"
    "${PROJECT_SOURCE_DIR}/tools/envsetup.py")
set_tests_properties(neko.tooling.envsetup PROPERTIES TIMEOUT 30 LABELS tooling)
