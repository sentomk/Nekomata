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
  "nekomata-publisher-request/1\n"
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

# The browser flavor: the same tool publishes a `nekomata-wasm/1` offer
# behind an atomically replaced `latest`, with sequence-named artifacts.
set(wasm_root "${CMAKE_CURRENT_BINARY_DIR}/publisher-wasm-cli")
file(REMOVE_RECURSE "${wasm_root}")
file(MAKE_DIRECTORY "${wasm_root}")
file(WRITE "${wasm_root}/module.wasm" "wasm-bytes")

function(publish_wasm expect_sequence)
  execute_process(
    COMMAND "${PUBLISHER}" wasm
      --root "${wasm_root}" --key "cli-e2e" --group "//cli:ball"
      --abi "wasm32-ball-v1" --module "${wasm_root}/module.wasm"
      --entry "identify" --entry "update_world"
    RESULT_VARIABLE wasm_status OUTPUT_VARIABLE wasm_output ERROR_VARIABLE wasm_error TIMEOUT 30)
  if(NOT "${wasm_status}" EQUAL 0)
    message(FATAL_ERROR "wasm publisher failed (${wasm_status}):\n${wasm_output}${wasm_error}")
  endif()
  string(REGEX MATCH "^([0-9]+) (g-[0-9a-f]+)" wasm_matched "${wasm_output}")
  if(NOT wasm_matched)
    message(FATAL_ERROR "unexpected wasm publisher output:\n${wasm_output}")
  endif()
  if(NOT "${CMAKE_MATCH_1}" STREQUAL "${expect_sequence}")
    message(FATAL_ERROR "expected wasm sequence ${expect_sequence}, got ${CMAKE_MATCH_1}")
  endif()
  set(wasm_generation_id "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

publish_wasm(1)
file(WRITE "${wasm_root}/module.wasm" "wasm-bytes-2")
publish_wasm(2)

file(READ "${wasm_root}/latest" wasm_offer_text)
foreach(wasm_required IN ITEMS
    "nekomata-wasm/1"
    "group_id \"//cli:ball\""
    "sequence 2"
    "abi_id \"wasm32-ball-v1\""
    "entry \"identify\""
    "entry \"update_world\"")
  string(FIND "${wasm_offer_text}" "${wasm_required}" wasm_at)
  if(wasm_at EQUAL -1)
    message(FATAL_ERROR "missing '${wasm_required}' in wasm offer:\n${wasm_offer_text}")
  endif()
endforeach()

# The manifest names the sequence-named immutable artifact, and both
# published artifacts survive the replacement of `latest`.
file(GLOB wasm_artifacts "${wasm_root}/modules/*.wasm")
list(LENGTH wasm_artifacts wasm_artifact_count)
if(NOT wasm_artifact_count EQUAL 2)
  message(FATAL_ERROR "expected 2 immutable artifacts, got ${wasm_artifact_count}: ${wasm_artifacts}")
endif()
string(REGEX MATCH "\"modules/2-${wasm_generation_id}.wasm\"" wasm_artifact_row "${wasm_offer_text}")
if(NOT wasm_artifact_row)
  message(FATAL_ERROR "offer does not name its sequence-named artifact:\n${wasm_offer_text}")
endif()
if(NOT EXISTS "${wasm_root}/modules/2-${wasm_generation_id}.wasm")
  message(FATAL_ERROR "sequence-named artifact missing: modules/2-${wasm_generation_id}.wasm")
endif()

file(GLOB markers "${root}/cli-e2e/offers/*.ready")
list(LENGTH markers marker_count)
if(NOT marker_count EQUAL 2)
  message(FATAL_ERROR "expected 2 ready markers, got ${marker_count}: ${markers}")
endif()

file(READ "${root}/cli-e2e/generations/${generation_id}/manifest" manifest)
foreach(required IN ITEMS
    "nekomata-generation/2"
    "group_id \"//cli:hot\""
    "sequence 2"
    "member \"cli/a\" \"objects/cli/a.o\""
    "member \"cli/b\" \"objects/cli/b.o\"")
  string(FIND "${manifest}" "${required}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing '${required}' in manifest:\n${manifest}")
  endif()
endforeach()
