nekomata_add_unit_test(
  NAME contract.public_api.smoke
  SOURCES smoke.cpp
  LIBRARIES nekomata::neko)

# The public entry points work without the umbrella or private headers; the
# output gate additionally proves the expected banner and no sanitizer noise.
add_executable(neko_public_api public_api.cpp)
target_include_directories(neko_public_api PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_link_libraries(neko_public_api PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_public_api)
nekomata_apply_sanitizers(neko_public_api)
add_test(NAME neko.contract.public_api.entries
  COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:neko_public_api>"
    "-DEXPECTED=public API linked: ${PROJECT_VERSION} (42)"
    -P "${PROJECT_SOURCE_DIR}/tests/support/cmake/check_output.cmake")
set_tests_properties(neko.contract.public_api.entries PROPERTIES TIMEOUT 15 LABELS contract)
