file(SHA256 "${FIXTURE}" before)
execute_process(COMMAND "${INSPECTOR}" inspect "${FIXTURE}"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT output MATCHES "summary: 3 compilation unit" OR NOT error STREQUAL "")
  message(FATAL_ERROR "inspect failed: ${result}\n${error}\n${output}")
endif()
string(REGEX MATCHALL "match=matched" matches "${output}")
list(LENGTH matches matched_count)
if(NOT matched_count EQUAL 10 OR
   NOT output MATCHES "association: 10 matched, 0 unresolved; symtab=present" OR
   NOT output MATCHES "candidate symbol table=[0-9]+ index=[0-9]+ section=[0-9]+")
  message(FATAL_ERROR "missing or incorrect ELF associations:\n${output}")
endif()
string(REGEX MATCHALL "declaration file=" declarations "${output}")
list(LENGTH declarations declaration_count)
if(NOT declaration_count EQUAL 10 OR
   NOT output MATCHES "declaration file=\"[^\"\n]*/a.cpp\" line=3 column=" OR
   NOT output MATCHES "declaration file=\"[^\"\n]*/b.cpp\" line=3 column=")
  message(FATAL_ERROR "missing or incorrect declaration coordinates:\n${output}")
endif()
file(SHA256 "${FIXTURE}" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "inspect modified the binary")
endif()

# A successful inspection reports ambiguity; success is not patch approval.
execute_process(COMMAND "${INSPECTOR}" inspect "${ALIASES}"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT error STREQUAL "" OR
   NOT output MATCHES "match=ambiguous" OR NOT output MATCHES "name=\"alias_one\"" OR
   NOT output MATCHES "name=\"alias_two\"")
  message(FATAL_ERROR "alias ambiguity was lost:\n${error}\n${output}")
endif()

function(expect_error path expected)
  execute_process(COMMAND "${INSPECTOR}" inspect "${path}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
  if(NOT result EQUAL 1 OR NOT output STREQUAL "" OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "unexpected rejection for ${path}: ${result}\n${error}\n${output}")
  endif()
endfunction()
expect_error("${NO_DEBUG}" "no embedded DWARF")
# DWARF 5 is what GCC and Clang emit by default, so it has to inspect rather
# than refuse. Same sources, same expectations: the version is not supposed to
# change what the binary says about itself.
execute_process(COMMAND "${INSPECTOR}" inspect "${DWARF5}"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT error STREQUAL "")
  message(FATAL_ERROR "DWARF 5 inspection failed: ${result}\n${error}\n${output}")
endif()
string(REGEX MATCHALL "match=matched" matches "${output}")
list(LENGTH matches matched_count)
if(NOT output MATCHES "summary: 3 compilation unit" OR NOT matched_count EQUAL 10)
  message(FATAL_ERROR "DWARF 5 listing differs from DWARF 4:\n${output}")
endif()
expect_error("${PIE}" "expected ET_EXEC")
expect_error("${FIXTURE}.missing" "cannot open binary")

execute_process(COMMAND "${INSPECTOR}" inspect RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 2 OR NOT error MATCHES "Usage:")
  message(FATAL_ERROR "missing inspect argument should be a usage error")
endif()
