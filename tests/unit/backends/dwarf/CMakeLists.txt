# DWARF reader unit suites. Fixture flags are explicit, independent of
# Debug/Release and sanitizers.
add_executable(neko_dwarf_fixture fixtures/main.cpp fixtures/a.cpp fixtures/b.cpp)
target_compile_options(neko_dwarf_fixture PRIVATE
  -O0 -g -gdwarf-4 -fno-inline -fno-pie -fno-lto)
target_link_options(neko_dwarf_fixture PRIVATE -no-pie -fno-lto)
nekomata_apply_warnings(neko_dwarf_fixture)

add_executable(neko_index_alias_fixture fixtures/aliases.cpp)
target_compile_options(neko_index_alias_fixture PRIVATE
  -O0 -g -gdwarf-4 -fno-inline -fno-pie -fno-lto)
target_link_options(neko_index_alias_fixture PRIVATE -no-pie -fno-lto)
nekomata_apply_warnings(neko_index_alias_fixture)

# The same sources as the DWARF 4 fixture, compiled with the version GCC and
# Clang emit by default. Same expectations: the version says nothing about what
# the program contains, so a difference here is a bug in the reader.
add_executable(neko_dwarf_fixture_v5 fixtures/main.cpp fixtures/a.cpp fixtures/b.cpp)
target_compile_options(neko_dwarf_fixture_v5 PRIVATE
  -O0 -g -gdwarf-5 -fno-inline -fno-pie -fno-lto)
target_link_options(neko_dwarf_fixture_v5 PRIVATE -no-pie -fno-lto)
nekomata_apply_warnings(neko_dwarf_fixture_v5)

nekomata_add_unit_test(
  NAME unit.backends.dwarf.function_index
  SOURCES function_index.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES neko_function_index
  TEST_ARGS $<TARGET_FILE:neko_dwarf_fixture> $<TARGET_FILE:neko_index_alias_fixture>)

nekomata_add_unit_test(
  NAME unit.backends.dwarf.source_locations
  SOURCES source_locations.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/elf/debug")
target_include_directories(neko_unit_backends_dwarf_source_locations SYSTEM PRIVATE
  $<TARGET_PROPERTY:libdwarf::dwarf-shared,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(neko_unit_backends_dwarf_source_locations PRIVATE nekomata::backends::dwarf)

foreach(kind IN ITEMS no_debug dwarf5 pie)
  add_executable(neko_dwarf_${kind} fixtures/bare.cpp)
  target_compile_options(neko_dwarf_${kind} PRIVATE -O0 -fno-lto)
  if(kind STREQUAL "pie")
    target_compile_options(neko_dwarf_${kind} PRIVATE -g -gdwarf-4 -fPIE)
    target_link_options(neko_dwarf_${kind} PRIVATE -pie -fno-lto)
  else()
    target_compile_options(neko_dwarf_${kind} PRIVATE -fno-pie)
    target_link_options(neko_dwarf_${kind} PRIVATE -no-pie -fno-lto)
    if(kind STREQUAL "no_debug")
      target_compile_options(neko_dwarf_${kind} PRIVATE -g0)
    else()
      target_compile_options(neko_dwarf_${kind} PRIVATE -g -gdwarf-5)
    endif()
  endif()
endforeach()

# Cross-checked against nm: the reference oracle.
add_executable(neko_dwarf_tests inspect.cpp)
target_include_directories(neko_dwarf_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/support"
  "${PROJECT_SOURCE_DIR}/src/backends/elf/debug")
target_include_directories(neko_dwarf_tests SYSTEM PRIVATE
  $<TARGET_PROPERTY:libdwarf::dwarf-shared,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(neko_dwarf_tests PRIVATE nekomata::backends::dwarf)
nekomata_apply_warnings(neko_dwarf_tests)
nekomata_apply_sanitizers(neko_dwarf_tests)

add_test(NAME neko.unit.backends.dwarf.inspect
  COMMAND "${CMAKE_COMMAND}"
    "-DNM=${CMAKE_NM}" "-DTESTER=$<TARGET_FILE:neko_dwarf_tests>"
    "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
    "-DREFERENCE=${CMAKE_CURRENT_BINARY_DIR}/fixture.nm"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/reference.cmake")
set_tests_properties(neko.unit.backends.dwarf.inspect PROPERTIES TIMEOUT 30)
