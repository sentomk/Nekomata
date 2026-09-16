if(NOT (NEKOMATA_TARGET_LINUX AND NEKOMATA_ENABLE_SANITIZERS))
  return()
endif()

# A canary for the sanitizer gate itself: reporting UB but returning zero
# must fail this test. Keep the intentionally invalid code out of fixtures.
add_executable(neko_sanitizer_probe sanitizer_probe.cpp)
target_link_libraries(neko_sanitizer_probe PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_sanitizer_probe)
nekomata_apply_sanitizers(neko_sanitizer_probe)
add_test(NAME neko.tooling.sanitizer.failure
  COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:neko_sanitizer_probe>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/sanitizer.cmake")
set_tests_properties(neko.tooling.sanitizer.failure PROPERTIES TIMEOUT 10
  LABELS "tooling;linux;sanitizer")
