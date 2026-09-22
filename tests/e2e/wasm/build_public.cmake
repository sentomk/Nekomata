# Build and install the real library before compiling an ordinary package consumer.
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Public WASM consumer build failed: ${ARGV}")
  endif()
endfunction()

set(prefix "${BINARY_ROOT}/public-install")
run("${EMCMAKE}" "${CMAKE_COMMAND}" -S "${SOURCE_ROOT}" -B "${BINARY_ROOT}/public-library"
  -G Ninja "-DCMAKE_MAKE_PROGRAM=${NINJA}" -DCMAKE_BUILD_TYPE=Debug
  "-DCMAKE_INSTALL_PREFIX=${prefix}"
  -DNEKOMATA_BUILD_TESTS=OFF -DNEKOMATA_BUILD_TOOLS=OFF -DNEKOMATA_BUILD_EXAMPLES=OFF)
run("${CMAKE_COMMAND}" --build "${BINARY_ROOT}/public-library" --target install --parallel 2)
run("${EMCMAKE}" "${CMAKE_COMMAND}" -S "${SOURCE_ROOT}/tests/e2e/wasm/public_project"
  -B "${BINARY_ROOT}/public-consumer" -G Ninja "-DCMAKE_MAKE_PROGRAM=${NINJA}"
  -DCMAKE_BUILD_TYPE=Debug "-Dnekomata_DIR=${prefix}/lib/cmake/nekomata" "-DFIXTURE_OUTPUT=${BINARY_ROOT}"
  "-DNEKOMATA_PUBLISHER_EXECUTABLE=${PUBLISHER}")
run("${CMAKE_COMMAND}" --build "${BINARY_ROOT}/public-consumer" --parallel 2)
