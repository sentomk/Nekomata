execute_process(COMMAND "${PROBE}"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(result EQUAL 0 OR NOT error MATCHES "runtime error: signed integer overflow")
  message(FATAL_ERROR "UBSan must diagnose AND fail the process: ${result}\n${output}\n${error}")
endif()
