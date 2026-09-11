# The manifest is the offline half of symbol disambiguation: it answers "which
# source file produced this address?" for a program that must not link a DWARF
# reader itself. The test pins the property the runtime depends on — the same
# symbol name appearing once per source file, at different addresses — because
# without it there is nothing to disambiguate with.
execute_process(COMMAND "${CLI}" manifest "${FIXTURE}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 10)
if(NOT "${status}" STREQUAL "0")
  message(FATAL_ERROR "manifest failed (${status}):\n${output}${error}")
endif()

foreach(required IN ITEMS
    "# nekomata symbol manifest v1"
    "# link-time addresses; regenerate after relinking")
  string(FIND "${output}" "${required}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing '${required}' in:\n${output}")
  endif()
endforeach()

# One line per matched symbol: address, symbol, source file.
string(REGEX MATCHALL "0x[0-9a-f]+\t[^\t\n]+\t[^\n]+" entries "${output}")
list(LENGTH entries entry_count)
if(entry_count LESS 4)
  message(FATAL_ERROR "expected several matched symbols, got ${entry_count}:\n${output}")
endif()

# The point of the whole exercise: a file-static helper compiled into two
# translation units lands at two addresses with the same name, and the source
# file is what tells them apart.
string(REGEX MATCHALL "[^\n]*_ZL6helperv[^\n]*" helpers "${output}")
list(LENGTH helpers helper_count)
if(NOT helper_count EQUAL 2)
  message(FATAL_ERROR "expected the static helper twice, got ${helper_count}:\n${output}")
endif()
if(NOT "${helpers}" MATCHES "a\\.cpp" OR NOT "${helpers}" MATCHES "b\\.cpp")
  message(FATAL_ERROR "helpers do not name both source files:\n${helpers}")
endif()
string(REPLACE "\n" ";" helper_lines "${helpers}")
list(GET helper_lines 0 first_helper)
list(GET helper_lines 1 second_helper)
string(REGEX REPLACE "\t.*" "" first_address "${first_helper}")
string(REGEX REPLACE "\t.*" "" second_address "${second_helper}")
if(first_address STREQUAL second_address)
  message(FATAL_ERROR "the two helpers share an address ${first_address}:\n${helpers}")
endif()
