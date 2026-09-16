if(NOT TARGET nekomata)
  return()
endif()

# The manifest needs the offline DWARF inspector and a fixture binary built
# with debug information; both come with the DWARF unit suites.
if(TARGET neko_backend_dwarf)
  add_test(NAME neko.tooling.cli.manifest
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:nekomata>"
      "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_manifest.cmake")
  set_tests_properties(neko.tooling.cli.manifest PROPERTIES TIMEOUT 20
    LABELS "tooling;linux")

  # The full inspect behavior matrix, against the same fixtures.
  add_test(NAME neko.tooling.cli.dwarf
    COMMAND "${CMAKE_COMMAND}"
      "-DINSPECTOR=$<TARGET_FILE:nekomata>"
      "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
      "-DNO_DEBUG=$<TARGET_FILE:neko_dwarf_no_debug>"
      "-DDWARF5=$<TARGET_FILE:neko_dwarf_fixture_v5>"
      "-DPIE=$<TARGET_FILE:neko_dwarf_pie>"
      "-DALIASES=$<TARGET_FILE:neko_index_alias_fixture>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/dwarf.cmake")
  set_tests_properties(neko.tooling.cli.dwarf PROPERTIES TIMEOUT 30
    LABELS "tooling;linux")
endif()

if(NOT TARGET neko_backend_dwarf)
  # DWARF disabled: the CLI must say so instead of half-working.
  add_test(NAME neko.tooling.cli.without_dwarf
    COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:nekomata>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_without_dwarf.cmake")
  set_tests_properties(neko.tooling.cli.without_dwarf PROPERTIES TIMEOUT 10
    LABELS tooling)
endif()
