# PE backend unit suites. The COFF reader parses bytes offline against real
# compiler output (no linking or executing of fixtures); the VirtualAlloc
# suites stay Windows-gated. Fixture flags stay explicit here — they are test
# inputs (see nekomata_test.cmake).

if(NEKOMATA_TARGET_WINDOWS)
  # clang-cl delivers hardware exceptions into __except only under /EHa
  # (MSVC accepts them under /EHsc too); the guard test observes the fault.
  set_source_files_properties(code_pages.cpp PROPERTIES COMPILE_OPTIONS "/EHa")

  nekomata_add_unit_test(
    NAME unit.backends.pe.code_pages
    SOURCES code_pages.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/runtime"
    LIBRARIES neko_backend_pe)
endif()

# POSIX hosts cross-compile the fixtures through the windows-msvc target;
# Windows hosts produce COFF natively at the drivers' /Od defaults. GCC
# cannot emit COFF, so those hosts skip the suite — both CI Windows jobs and
# every Clang host still cover it.
if(WIN32 OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(neko_coff_cross "")
  if(NOT WIN32)
    # Newer CMake injects the host -arch into AppleClang flags; the target
    # wins but the leftover argument raises a driver warning under -Werror.
    set(neko_coff_cross --target=x86_64-pc-windows-msvc -Wno-unused-command-line-argument)
  endif()

  add_library(neko_pe_object_basic OBJECT fixtures/basic.c)
  target_compile_options(neko_pe_object_basic PRIVATE ${neko_coff_cross})
  nekomata_apply_warnings(neko_pe_object_basic)

  add_library(neko_pe_object_shapes OBJECT fixtures/shapes.cpp)
  if(MSVC)
    target_compile_options(neko_pe_object_shapes PRIVATE /EHsc)
  else()
    target_compile_options(neko_pe_object_shapes PRIVATE ${neko_coff_cross} -nostdinc++)
  endif()
  nekomata_apply_warnings(neko_pe_object_shapes)

  nekomata_add_unit_test(
    NAME unit.backends.pe.object_sections
    SOURCES object_sections.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/format"
    LIBRARIES neko_pe_file
    COMMAND_EXPAND_LISTS
    TEST_ARGS $<TARGET_OBJECTS:neko_pe_object_basic> $<TARGET_OBJECTS:neko_pe_object_shapes>)

  nekomata_add_unit_test(
    NAME unit.backends.pe.object_symbols
    SOURCES object_symbols.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/format"
    LIBRARIES neko_pe_file
    COMMAND_EXPAND_LISTS
    TEST_ARGS $<TARGET_OBJECTS:neko_pe_object_basic> $<TARGET_OBJECTS:neko_pe_object_shapes>)
  add_dependencies(neko_unit_backends_pe_object_sections
    neko_pe_object_basic neko_pe_object_shapes)
  add_dependencies(neko_unit_backends_pe_object_symbols
    neko_pe_object_basic neko_pe_object_shapes)

  add_library(neko_pe_object_relocs OBJECT fixtures/relocs.c)
  target_compile_options(neko_pe_object_relocs PRIVATE ${neko_coff_cross})
  nekomata_apply_warnings(neko_pe_object_relocs)

  # Debug streams carry SECREL/SECTION relocations; CodeView on MSVC (/Z7)
  # and clang (-gcodeview through the cross target).
  add_library(neko_pe_object_debug OBJECT fixtures/shapes.cpp)
  if(MSVC)
    target_compile_options(neko_pe_object_debug PRIVATE /EHsc /Z7)
  else()
    target_compile_options(neko_pe_object_debug PRIVATE
      ${neko_coff_cross} -nostdinc++ -gcodeview)
  endif()
  nekomata_apply_warnings(neko_pe_object_debug)

  nekomata_add_unit_test(
    NAME unit.backends.pe.object_relocations
    SOURCES object_relocations.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/format"
    LIBRARIES neko_pe_file
    COMMAND_EXPAND_LISTS
    TEST_ARGS $<TARGET_OBJECTS:neko_pe_object_relocs> $<TARGET_OBJECTS:neko_pe_object_shapes>
               $<TARGET_OBJECTS:neko_pe_object_debug>)
  add_dependencies(neko_unit_backends_pe_object_relocations
    neko_pe_object_relocs neko_pe_object_shapes neko_pe_object_debug)

  nekomata_add_unit_test(
    NAME unit.backends.pe.image_layout
    SOURCES image_layout.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/format"
    LIBRARIES neko_pe_file
    COMMAND_EXPAND_LISTS
    TEST_ARGS $<TARGET_OBJECTS:neko_pe_object_relocs> $<TARGET_OBJECTS:neko_pe_object_shapes>
               $<TARGET_OBJECTS:neko_pe_object_debug>)
  add_dependencies(neko_unit_backends_pe_image_layout
    neko_pe_object_relocs neko_pe_object_shapes neko_pe_object_debug)
endif()
