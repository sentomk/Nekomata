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
      --member "cli/a" "${root}/in/a.o" "//cli:a.cpp" "-O0 diagnostic"
      --member "cli/b" "${root}/in/b.o" "//cli:b.cpp" "-O0 diagnostic"
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
