# The CMake adapter end to end: a fixture project consumed through
# cmake/nekomata.cmake. One case per (generator, group form) cell so a
# failure names the exact combination. Kept bespoke: no runner of its own.
configure_file(run.sh.in "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh" @ONLY)

set(neko_matrix_cells Ninja ninja "Unix Makefiles" unix_makefiles)
while(NOT neko_matrix_cells STREQUAL "")
  list(POP_FRONT neko_matrix_cells neko_generator neko_tag)
  foreach(neko_scope IN ITEMS sources units)
    add_test(NAME neko.e2e.adapters.cmake.${neko_tag}.${neko_scope}
      COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh"
        "${neko_generator}" "${neko_scope}")
    set_tests_properties(neko.e2e.adapters.cmake.${neko_tag}.${neko_scope}
      PROPERTIES TIMEOUT 900 LABELS "e2e;linux;slow;cmake")
  endforeach()
endwhile()
