execute_process(COMMAND "${NM}" -S -n --defined-only "${FIXTURE}"
  OUTPUT_FILE "${REFERENCE}" ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "nm failed: ${error}")
endif()
execute_process(COMMAND "${TESTER}" "${FIXTURE}" "${REFERENCE}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "inspection assertions failed: ${result}")
endif()
