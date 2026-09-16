# Installs the build into a scratch prefix, then configures, builds, and
# runs the consumer project against it through find_package only.
set(work "${BINARY_DIR}/consumer-check")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
  --prefix "${work}/prefix" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "install failed (${status})")
endif()

file(COPY "${CONSUMER_DIR}/" DESTINATION "${work}/consumer")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${work}/consumer" -B "${work}/consumer-build"
  "-DCMAKE_PREFIX_PATH=${work}/prefix"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "consumer configure failed (${status})")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/consumer-build"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "consumer build failed (${status})")
endif()

execute_process(COMMAND "${work}/consumer-build/consumer"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${status}):\n${output}${error}")
endif()
if(NOT output MATCHES "consumer linked and ran")
  message(FATAL_ERROR "unexpected consumer output:\n${output}${error}")
endif()
message(STATUS "installed-package consumer OK")
