# The CMake adapter end to end: a fixture project consumed through
# cmake/nekomata.cmake. Kept bespoke: no runner of its own, and the
# generator x form matrix is split into independent cases.
configure_file(run.sh.in "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh" @ONLY)
add_test(NAME neko.e2e.build.cmake
  COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh")
set_tests_properties(neko.e2e.build.cmake PROPERTIES TIMEOUT 900
  LABELS "e2e;linux;slow;cmake")
