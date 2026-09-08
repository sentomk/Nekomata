add_executable(neko_elf_file_tests binary_file.cpp)
target_include_directories(neko_elf_file_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_elf_file_tests PRIVATE neko_elf_file)
nekomata_apply_warnings(neko_elf_file_tests)
nekomata_apply_sanitizers(neko_elf_file_tests)
add_test(NAME neko.elf.file COMMAND neko_elf_file_tests)
set_tests_properties(neko.elf.file PROPERTIES TIMEOUT 30)

# Inspect actual compiler output without linking or executing the fixtures.
# These also run with DWARF inspection disabled in the build matrix.
foreach(kind IN ITEMS plain sections)
  add_library(neko_elf_object_${kind} OBJECT fixtures/a.cpp fixtures/b.cpp)
  target_compile_options(neko_elf_object_${kind} PRIVATE
    -O0 -g0 -fno-inline -fno-pie -fno-lto -fno-function-sections)
  if(kind STREQUAL "sections")
    target_compile_options(neko_elf_object_${kind} PRIVATE -ffunction-sections)
  endif()
  nekomata_apply_warnings(neko_elf_object_${kind})
endforeach()

add_executable(neko_elf_object_tests object_symbols.cpp)
target_include_directories(neko_elf_object_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_elf_object_tests PRIVATE neko_elf_file)
nekomata_apply_warnings(neko_elf_object_tests)
nekomata_apply_sanitizers(neko_elf_object_tests)
add_dependencies(neko_elf_object_tests neko_elf_object_plain neko_elf_object_sections)
add_test(NAME neko.elf.object_symbols COMMAND neko_elf_object_tests
  $<TARGET_OBJECTS:neko_elf_object_plain> $<TARGET_OBJECTS:neko_elf_object_sections>
  COMMAND_EXPAND_LISTS)
set_tests_properties(neko.elf.object_symbols PROPERTIES TIMEOUT 30)

add_library(neko_elf_debug_objects OBJECT fixtures/a.cpp fixtures/b.cpp)
target_compile_options(neko_elf_debug_objects PRIVATE
  -O0 -g -gdwarf-4 -fno-inline -fno-pie -fno-lto -ffunction-sections)
nekomata_apply_warnings(neko_elf_debug_objects)
add_executable(neko_elf_relocation_tests debug_relocations.cpp)
target_include_directories(neko_elf_relocation_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_elf_relocation_tests PRIVATE neko_elf_file)
nekomata_apply_warnings(neko_elf_relocation_tests)
nekomata_apply_sanitizers(neko_elf_relocation_tests)
add_dependencies(neko_elf_relocation_tests neko_elf_debug_objects)
add_test(NAME neko.elf.debug_relocations COMMAND neko_elf_relocation_tests
  $<TARGET_OBJECTS:neko_elf_debug_objects> COMMAND_EXPAND_LISTS)
set_tests_properties(neko.elf.debug_relocations PROPERTIES TIMEOUT 30)

add_executable(neko_elf_guard_tests code_pages_guard.cpp)
target_include_directories(neko_elf_guard_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_elf_guard_tests PRIVATE neko_backend_elf)
nekomata_apply_warnings(neko_elf_guard_tests)
nekomata_apply_sanitizers(neko_elf_guard_tests)
add_test(NAME neko.elf.guard_pages COMMAND neko_elf_guard_tests)
set_tests_properties(neko.elf.guard_pages PROPERTIES TIMEOUT 30)
