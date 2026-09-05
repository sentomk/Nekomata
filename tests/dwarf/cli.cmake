file(SHA256 "${FIXTURE}" before)
execute_process(COMMAND "${INSPECTOR}" inspect "${FIXTURE}"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT output MATCHES "summary: 3 compilation unit" OR NOT error STREQUAL "")
  message(FATAL_ERROR "inspect failed: ${result}\n${error}\n${output}")
endif()
file(SHA256 "${FIXTURE}" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "inspect modified the binary")
endif()

function(expect_error path expected)
  execute_process(COMMAND "${INSPECTOR}" inspect "${path}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
  if(NOT result EQUAL 1 OR NOT output STREQUAL "" OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "unexpected rejection for ${path}: ${result}\n${error}\n${output}")
  endif()
endfunction()
expect_error("${NO_DEBUG}" "no embedded DWARF")
expect_error("${DWARF5}" "unsupported compilation unit")
expect_error("${PIE}" "expected ET_EXEC")
expect_error("${FIXTURE}.missing" "cannot open binary")

execute_process(COMMAND "${INSPECTOR}" inspect RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 2 OR NOT error MATCHES "Usage:")
  message(FATAL_ERROR "missing inspect argument should be a usage error")
endif()
