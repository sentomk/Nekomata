if(NOT UNIX)
  return()
endif()

# Installs this build into a scratch prefix and consumes it through
# find_package, proving the package needs no source-tree paths.
add_test(NAME neko.tooling.install.consumer
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DCONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/install_consumer.cmake")
set_tests_properties(neko.tooling.install.consumer PROPERTIES TIMEOUT 300
  LABELS "tooling;slow")
