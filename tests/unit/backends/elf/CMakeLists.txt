# ELF backend unit suites: offline parsing against real compiler output,
# no linking or executing of fixtures. Fixture flags stay explicit here —
# they are test inputs (see nekomata_test.cmake).

set(neko_unit_include_dirs
  "${PROJECT_SOURCE_DIR}/tests/support"
  "${PROJECT_SOURCE_DIR}/src/backends/elf/format"
  "${PROJECT_SOURCE_DIR}/src/backends/elf/runtime")

nekomata_add_unit_test(
  NAME unit.backends.elf.binary_file
  SOURCES binary_file.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_elf_file)

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

nekomata_add_unit_test(
  NAME unit.backends.elf.object_symbols
  SOURCES object_symbols.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_elf_file
  COMMAND_EXPAND_LISTS
  TEST_ARGS $<TARGET_OBJECTS:neko_elf_object_plain> $<TARGET_OBJECTS:neko_elf_object_sections>)
add_dependencies(neko_unit_backends_elf_object_symbols
  neko_elf_object_plain neko_elf_object_sections)

add_library(neko_elf_debug_objects OBJECT fixtures/a.cpp fixtures/b.cpp)
target_compile_options(neko_elf_debug_objects PRIVATE
  -O0 -g -gdwarf-4 -fno-inline -fno-pie -fno-lto -ffunction-sections)
nekomata_apply_warnings(neko_elf_debug_objects)

nekomata_add_unit_test(
  NAME unit.backends.elf.debug_relocations
  SOURCES debug_relocations.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_elf_file
  COMMAND_EXPAND_LISTS
  TEST_ARGS $<TARGET_OBJECTS:neko_elf_debug_objects>)
add_dependencies(neko_unit_backends_elf_debug_relocations neko_elf_debug_objects)

nekomata_add_unit_test(
  NAME unit.backends.elf.guard_pages
  SOURCES code_pages_guard.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_backend_elf)

# Trampoline regression: variadic callees rely on AL surviving the PLT stub.
add_library(neko_elf_trampoline_object OBJECT fixtures/plt_call.cpp)
target_compile_options(neko_elf_trampoline_object PRIVATE
  -O0 -g0 -fno-inline -fno-pie -fno-lto -fno-function-sections)
nekomata_apply_warnings(neko_elf_trampoline_object)

nekomata_add_unit_test(
  NAME unit.backends.elf.plt_trampoline
  SOURCES plt_trampoline.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_backend_elf
  COMMAND_EXPAND_LISTS
  TEST_ARGS $<TARGET_OBJECTS:neko_elf_trampoline_object>)
add_dependencies(neko_unit_backends_elf_plt_trampoline neko_elf_trampoline_object)
