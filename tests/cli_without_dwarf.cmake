execute_process(COMMAND "${CLI}" inspect unused-binary
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 1 OR NOT output STREQUAL "" OR NOT error MATCHES "inspect unavailable")
  message(FATAL_ERROR "disabled inspector should fail explicitly: ${result}\n${output}\n${error}")
endif()
