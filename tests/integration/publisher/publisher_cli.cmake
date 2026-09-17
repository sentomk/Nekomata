# End-to-end check of the private publisher executable: two publications
# must produce strictly increasing sequences, valid ready markers, and
# manifests the runtime protocol can parse.
set(root "${CMAKE_CURRENT_BINARY_DIR}/publisher-cli")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/in")
file(WRITE "${root}/in/a.o" "object-a")
file(WRITE "${root}/in/b.o" "object-b")

function(publish expect_sequence)
  execute_process(
    COMMAND "${PUBLISHER}"
      --root "${root}" --key "cli-e2e" --group "//cli:hot"
      --compat "sha256:compile-identity" --abi "elf-x86_64-patch-v1"
      --changed "//cli:a.cpp"
      --member "cli/a" "//cli:a.cpp" "-O0 diagnostic"
      --member "cli/b" "//cli:b.cpp" "-O0 diagnostic"
      --objects "${root}/in/a.o" "${root}/in/b.o"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
  if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "publisher failed (${status}):\n${output}${error}")
  endif()
  string(REGEX MATCH "^([0-9]+) (g-[0-9a-f]+)" matched "${output}")
  if(NOT matched)
    message(FATAL_ERROR "unexpected publisher output:\n${output}")
  endif()
  if(NOT "${CMAKE_MATCH_1}" STREQUAL "${expect_sequence}")
    message(FATAL_ERROR "expected sequence ${expect_sequence}, got ${CMAKE_MATCH_1}")
  endif()
  set(generation_id "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

publish(1)
publish(2)

# A request larger than ARG_MAX must reach the publisher without becoming
# process arguments. One large diagnostic field keeps this regression fast.
string(REPEAT "x" 2200000 oversized_argument)
set(request_file "${root}/publish.request")
file(WRITE "${request_file}"
  "nekomata-publisher-request 1\r\n"
  "--root\n${root}\n"
  "--key\nrequest-e2e\n"
  "--group\n//cli:request\n"
  "--compat\nsha256:request-identity\n"
  "--abi\nelf-x86_64-patch-v1\n"
  "--member\ncli/large\n//cli:large.cpp\n${oversized_argument}\n"
  "--objects\n${root}/in/a.o\n")
execute_process(
  COMMAND "${PUBLISHER}" --request "${request_file}"
  RESULT_VARIABLE request_status OUTPUT_VARIABLE request_output
  ERROR_VARIABLE request_error TIMEOUT 30)
if(NOT "${request_status}" STREQUAL "0")
  message(FATAL_ERROR "request-file publisher failed (${request_status}):\n${request_error}")
endif()
string(REGEX MATCH "^1 (g-[0-9a-f]+)" request_matched "${request_output}")
if(NOT request_matched)
  message(FATAL_ERROR "unexpected request-file publisher output:\n${request_output}")
endif()
unset(oversized_argument)

file(GLOB markers "${root}/cli-e2e/offers/*.ready")
list(LENGTH markers marker_count)
if(NOT marker_count EQUAL 2)
  message(FATAL_ERROR "expected 2 ready markers, got ${marker_count}: ${markers}")
endif()

file(READ "${root}/cli-e2e/generations/${generation_id}/manifest" manifest)
foreach(required IN ITEMS
    "nekomata-generation-v2"
    "group_id \"//cli:hot\""
    "sequence 2"
    "member \"cli/a\" \"objects/cli/a.o\""
    "member \"cli/b\" \"objects/cli/b.o\"")
  string(FIND "${manifest}" "${required}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing '${required}' in manifest:\n${manifest}")
  endif()
endforeach()
