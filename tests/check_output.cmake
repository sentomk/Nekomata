# Output alone must never turn a failing executable into a passing test.
execute_process(COMMAND "${PROGRAM}" ${PROGRAM_ARGS}
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 10)
if(NOT "${status}" STREQUAL "0")
  message(FATAL_ERROR "Program failed (${status}):\n${output}${error}")
endif()
string(FIND "${output}${error}" "${EXPECTED}" position)
if(position EQUAL -1)
  message(FATAL_ERROR "Missing expected output '${EXPECTED}':\n${output}${error}")
endif()
if("${output}${error}" MATCHES "AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:")
  message(FATAL_ERROR "Sanitizer diagnostic:\n${output}${error}")
endif()
