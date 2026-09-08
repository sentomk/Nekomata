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

add_subdirectory(headers)
add_subdirectory(harness)

if(TARGET nekomata AND NOT TARGET neko_backend_dwarf)
  add_test(NAME neko.cli.without_dwarf
    COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:nekomata>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_without_dwarf.cmake")
  set_tests_properties(neko.cli.without_dwarf PROPERTIES TIMEOUT 10)
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
  add_subdirectory(duplicate_names)
  if(NEKOMATA_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_subdirectory(fuzz)
  endif()
  if(TARGET neko_backend_dwarf)
    add_subdirectory(dwarf)
  endif()
endif()
