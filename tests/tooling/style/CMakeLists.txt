# Source-level conventions that formatters cannot express.
add_test(NAME neko.tooling.style.comments
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/comment_style.cmake")
set_tests_properties(neko.tooling.style.comments PROPERTIES TIMEOUT 30
  LABELS tooling)
