# Fixture flags are explicit, independent of Debug/Release and sanitizers.
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

add_executable(neko_function_index_tests function_index.cpp)
target_include_directories(neko_function_index_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_function_index_tests PRIVATE neko_function_index)
nekomata_apply_warnings(neko_function_index_tests)
nekomata_apply_sanitizers(neko_function_index_tests)
add_test(NAME neko.elf.function_index COMMAND neko_function_index_tests
  $<TARGET_FILE:neko_dwarf_fixture> $<TARGET_FILE:neko_index_alias_fixture>)
set_tests_properties(neko.elf.function_index PROPERTIES TIMEOUT 30)

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

add_executable(neko_dwarf_tests inspect.cpp)
target_include_directories(neko_dwarf_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/dwarf")
target_link_libraries(neko_dwarf_tests PRIVATE nekomata::backends::dwarf)
nekomata_apply_warnings(neko_dwarf_tests)
nekomata_apply_sanitizers(neko_dwarf_tests)
# Match the other doctest targets: compiler warnings and sanitizers apply,
# while clang-tidy runs on the production reader and CLI, not doctest internals.

add_test(NAME neko.dwarf.inspect
  COMMAND "${CMAKE_COMMAND}"
    "-DNM=${CMAKE_NM}" "-DTESTER=$<TARGET_FILE:neko_dwarf_tests>"
    "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
    "-DREFERENCE=${CMAKE_CURRENT_BINARY_DIR}/fixture.nm"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/reference.cmake")
set_tests_properties(neko.dwarf.inspect PROPERTIES TIMEOUT 30)

if(TARGET nekomata)
  add_test(NAME neko.dwarf.cli
    COMMAND "${CMAKE_COMMAND}"
      "-DINSPECTOR=$<TARGET_FILE:nekomata>"
      "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
      "-DNO_DEBUG=$<TARGET_FILE:neko_dwarf_no_debug>"
      "-DDWARF5=$<TARGET_FILE:neko_dwarf_dwarf5>"
      "-DPIE=$<TARGET_FILE:neko_dwarf_pie>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")
  set_tests_properties(neko.dwarf.cli PROPERTIES TIMEOUT 30)
endif()
