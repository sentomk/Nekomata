# The module rules as an executable check: public headers stay
# self-contained, internal includes stay inside their module, and the C
# base layer is built by the C compiler.
add_test(NAME neko.tooling.boundaries
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/boundaries.cmake")
set_tests_properties(neko.tooling.boundaries PROPERTIES TIMEOUT 30
  LABELS tooling)
