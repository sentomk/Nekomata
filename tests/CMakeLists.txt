# Test suites, organized by verification level:
#   unit        one component, in-process; no compiler, no runner
#   contract    the public surface and its stable formats
#   integration several components over the real filesystem
#   e2e         compile, publish, and apply real generations
#   tooling     the development tools and scripts themselves
#   fuzz        sanitizer-driven parser targets
include("${CMAKE_CURRENT_SOURCE_DIR}/support/cmake/nekomata_test.cmake")

add_subdirectory(unit)
add_subdirectory(contract)
add_subdirectory(integration)
add_subdirectory(tooling)

if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(e2e)
  if(NEKOMATA_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_subdirectory(fuzz)
  endif()
endif()
