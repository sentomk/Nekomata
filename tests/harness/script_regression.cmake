# Each invocation uses a fresh child directory; logs survive for diagnosis.
string(RANDOM LENGTH 12 suffix)
set(work_dir "${WORK}/${suffix}")
file(MAKE_DIRECTORY "${work_dir}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "MOCK_CASE=${MODE}"
  "${BASH}" "${SCRIPT}" "${work_dir}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 40)
if(EXPECTED STREQUAL "")
  if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "Valid runner was rejected (${status}):\n${output}${error}")
  endif()
else()
  string(FIND "${output}${error}" "${EXPECTED}" position)
  # Timeout, configuration and launch failures must not masquerade as a
  # successful negative test: require the harness's explicit failure result.
  if(NOT "${status}" STREQUAL "1" OR position EQUAL -1)
    message(FATAL_ERROR "Harness did not reject ${MODE} correctly (${status}):\n${output}${error}")
  endif()
endif()
