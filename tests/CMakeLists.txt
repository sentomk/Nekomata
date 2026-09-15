# Tests use the vendored doctest single header (tests/vendor/doctest),
# so configuring never needs network access. Do not switch this back to
# FetchContent: dev machines and CI sandboxes behind restricted networks
# are a real scenario for this project (see commit "build: vendor doctest").
add_executable(neko_smoke smoke.cpp)
target_include_directories(neko_smoke
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_smoke PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_smoke)
nekomata_apply_sanitizers(neko_smoke)

add_test(NAME neko.smoke COMMAND neko_smoke)

add_executable(neko_public_api public_api.cpp)
target_link_libraries(neko_public_api PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_public_api)
nekomata_apply_sanitizers(neko_public_api)
add_test(NAME neko.public_api
  COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:neko_public_api>"
    "-DEXPECTED=public API linked: ${PROJECT_VERSION} (42)"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_output.cmake")
set_tests_properties(neko.public_api PROPERTIES TIMEOUT 15)

add_executable(neko_session_transaction session_transaction.cpp)
target_include_directories(neko_session_transaction
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_session_transaction PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_session_transaction)
nekomata_apply_sanitizers(neko_session_transaction)
add_test(NAME neko.session_transaction COMMAND neko_session_transaction)

add_executable(neko_depfile_planner depfile_planner.cpp)
target_include_directories(neko_depfile_planner
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_depfile_planner PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_depfile_planner)
nekomata_apply_sanitizers(neko_depfile_planner)
add_test(NAME neko.depfile_planner COMMAND neko_depfile_planner)

add_executable(neko_group_descriptor group_descriptor.cpp)
target_include_directories(neko_group_descriptor
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_group_descriptor PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_group_descriptor)
nekomata_apply_sanitizers(neko_group_descriptor)
add_test(NAME neko.group_descriptor COMMAND neko_group_descriptor)

add_executable(neko_generation_offer generation_offer.cpp)
target_include_directories(neko_generation_offer
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_generation_offer PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_generation_offer)
nekomata_apply_sanitizers(neko_generation_offer)
add_test(NAME neko.generation_offer COMMAND neko_generation_offer)

add_executable(neko_generation_stream generation_stream.cpp)
target_include_directories(neko_generation_stream
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_generation_stream PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_generation_stream)
nekomata_apply_sanitizers(neko_generation_stream)
add_test(NAME neko.generation_stream COMMAND neko_generation_stream)

add_executable(neko_sha256 sha256.cpp)
target_include_directories(neko_sha256
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_sha256 PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_sha256)
nekomata_apply_sanitizers(neko_sha256)
add_test(NAME neko.sha256 COMMAND neko_sha256)

add_executable(neko_publisher_tests publisher.cpp)
target_include_directories(neko_publisher_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_publisher_tests PRIVATE nekomata::neko neko_publisher_lib)
nekomata_apply_warnings(neko_publisher_tests)
nekomata_apply_sanitizers(neko_publisher_tests)
add_test(NAME neko.publisher COMMAND neko_publisher_tests)

if(TARGET neko_publisher)
  add_test(NAME neko.publisher_cli
    COMMAND "${CMAKE_COMMAND}"
      "-DPUBLISHER=$<TARGET_FILE:neko_publisher>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/publisher_cli.cmake")
  set_tests_properties(neko.publisher_cli PROPERTIES TIMEOUT 60)
endif()

add_executable(neko_descriptor_section descriptor_section.cpp)
target_include_directories(neko_descriptor_section
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_descriptor_section PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_descriptor_section)
nekomata_apply_sanitizers(neko_descriptor_section)
add_test(NAME neko.descriptor_section COMMAND neko_descriptor_section)

add_executable(neko_descriptor_discovery descriptor_discovery.cpp)
target_include_directories(neko_descriptor_discovery
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_descriptor_discovery PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_descriptor_discovery)
nekomata_apply_sanitizers(neko_descriptor_discovery)
add_test(NAME neko.descriptor_discovery COMMAND neko_descriptor_discovery)

add_executable(neko_managed_session managed_session.cpp)
target_include_directories(neko_managed_session
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_managed_session PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_managed_session)
nekomata_apply_sanitizers(neko_managed_session)
add_test(NAME neko.managed_session COMMAND neko_managed_session)

add_subdirectory(headers)
add_subdirectory(harness)
add_subdirectory(tooling)

# The manifest needs the offline DWARF inspector and a fixture binary built
# with debug information; both are Linux-only and optional.
if(TARGET nekomata AND TARGET neko_backend_dwarf)
  add_test(NAME neko.cli.manifest
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:nekomata>"
      "-DFIXTURE=$<TARGET_FILE:neko_dwarf_fixture>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_manifest.cmake")
  set_tests_properties(neko.cli.manifest PROPERTIES TIMEOUT 20)
endif()

if(TARGET nekomata AND NOT TARGET neko_backend_dwarf)
  add_test(NAME neko.cli.without_dwarf
    COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:nekomata>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_without_dwarf.cmake")
  set_tests_properties(neko.cli.without_dwarf PROPERTIES TIMEOUT 10)
endif()

if(NEKOMATA_TUI AND TARGET nekomata::tui)
  add_subdirectory(tui)
endif()

if(NEKOMATA_TARGET_LINUX)
  if(NEKOMATA_ENABLE_SANITIZERS)
    # A canary for the sanitizer gate itself: reporting UB but returning zero
    # must fail this test. Keep the intentionally invalid code out of fixtures.
    add_executable(neko_sanitizer_probe sanitizer_probe.cpp)
    target_link_libraries(neko_sanitizer_probe PRIVATE nekomata::neko)
    nekomata_apply_warnings(neko_sanitizer_probe)
    nekomata_apply_sanitizers(neko_sanitizer_probe)
    add_test(NAME neko.sanitizer.failure
      COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:neko_sanitizer_probe>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/sanitizer.cmake")
    set_tests_properties(neko.sanitizer.failure PROPERTIES TIMEOUT 10)
  endif()
  add_subdirectory(elf)
  add_subdirectory(rejections)
  add_subdirectory(soak)
  add_subdirectory(atomicity)
  add_subdirectory(multi_tu)
  add_subdirectory(duplicate_names)

  # Requires the linker's __start_/__stop_ section markers, which are an
  # ELF linking property.
  add_executable(neko_descriptor_embedded descriptor_embedded.cpp)
  target_include_directories(neko_descriptor_embedded
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
  target_link_libraries(neko_descriptor_embedded PRIVATE nekomata::neko)
  nekomata_apply_warnings(neko_descriptor_embedded)
  nekomata_apply_sanitizers(neko_descriptor_embedded)
  add_test(NAME neko.descriptor_embedded COMMAND neko_descriptor_embedded)

  # Managed watch end to end: embedded descriptor, published generations,
  # unwatch/resume semantics against a real linker section.
  add_executable(managed_reload_runner
    managed_reload/runner.cpp
    managed_reload/managed_hot.cpp)
  target_include_directories(managed_reload_runner PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
  target_link_libraries(managed_reload_runner PRIVATE nekomata::backends::elf)
  set_target_properties(managed_reload_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(managed_reload_runner PRIVATE -O0 -fno-pie -fno-pic)
  target_link_options(managed_reload_runner PRIVATE -no-pie)
  set_source_files_properties(managed_reload/managed_hot.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(managed_reload_runner)

  configure_file(managed_reload/managed_reload.sh.in
    "${CMAKE_CURRENT_BINARY_DIR}/managed-reload-run.sh" @ONLY)
  add_test(NAME neko.managed_reload
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/managed-reload-run.sh")
  set_tests_properties(neko.managed_reload PROPERTIES TIMEOUT 180)

  # Cross-TU candidate resolution: fresh objects call symbols defined by
  # sibling objects of the same generation, including brand-new ones.
  add_executable(cross_tu_runner
    cross_tu/runner.cpp
    cross_tu/a.cpp
    cross_tu/b.cpp)
  target_include_directories(cross_tu_runner PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/cross_tu"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
  target_link_libraries(cross_tu_runner PRIVATE nekomata::backends::elf)
  set_target_properties(cross_tu_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(cross_tu_runner PRIVATE -O0 -fno-pie -fno-pic)
  target_link_options(cross_tu_runner PRIVATE -no-pie)
  set_source_files_properties(cross_tu/a.cpp cross_tu/b.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
  nekomata_apply_warnings(cross_tu_runner)

  configure_file(cross_tu/cross_tu.sh.in
    "${CMAKE_CURRENT_BINARY_DIR}/cross-tu-run.sh" @ONLY)
  add_test(NAME neko.cross_tu
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/cross-tu-run.sh")
  set_tests_properties(neko.cross_tu PROPERTIES TIMEOUT 180)

  # The CMake adapter end to end: fixture project, two generators.
  configure_file(cmake_adapter/run.sh.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh" @ONLY)
  add_test(NAME neko.cmake_adapter
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/cmake-adapter-run.sh")
  set_tests_properties(neko.cmake_adapter PROPERTIES TIMEOUT 900)
  if(NEKOMATA_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_subdirectory(fuzz)
  endif()
  if(TARGET neko_backend_dwarf)
    add_subdirectory(dwarf)
  endif()
endif()
