# These fixtures cross-compile independently of the native project toolchain.
find_program(NEKOMATA_EMXX NAMES em++ REQUIRED)
find_program(NEKOMATA_EMCC NAMES emcc REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(NEKOMATA_WASM_BROWSER
  NAMES chromium chromium-browser google-chrome chrome
  PATHS "/Applications/Google Chrome.app/Contents/MacOS"
  NAMES_PER_DIR)
if(NOT NEKOMATA_WASM_BROWSER)
  find_program(NEKOMATA_WASM_BROWSER NAMES "Google Chrome"
    PATHS "/Applications/Google Chrome.app/Contents/MacOS" REQUIRED)
endif()

set(wasm_flags -std=c++20 -O0 -g -Wall -Wextra -Werror
  -fno-exceptions -fno-rtti -sASSERTIONS=2 "-I${PROJECT_SOURCE_DIR}/src")
set(wasm_backend "${PROJECT_SOURCE_DIR}/src/backends/wasm")
foreach(generation IN ITEMS a b incompatible incomplete)
  set(generation_defines -DINTERFACE_VERSION=1 -DMISSING_UPDATE=0)
  if(generation STREQUAL "a")
    set(generation_id 1)
  elseif(generation STREQUAL "b")
    set(generation_id 2)
  elseif(generation STREQUAL "incompatible")
    set(generation_id 3)
    set(generation_defines -DINTERFACE_VERSION=2 -DMISSING_UPDATE=0)
  else()
    set(generation_id 4)
    set(generation_defines -DINTERFACE_VERSION=1 -DMISSING_UPDATE=1)
  endif()
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${generation}.wasm"
    COMMAND "${NEKOMATA_EMXX}" ${wasm_flags} -sSIDE_MODULE=1
      "-DGENERATION_ID=${generation_id}" ${generation_defines}
      "${CMAKE_CURRENT_SOURCE_DIR}/generation.cpp"
      -o "${CMAKE_CURRENT_BINARY_DIR}/${generation}.wasm"
    DEPENDS generation.cpp contract.hpp
      "${wasm_backend}/module_descriptor.hpp" "${wasm_backend}/candidate.hpp" VERBATIM)
endforeach()
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
  COMMAND "${NEKOMATA_EMCC}" -c -Wall -Wextra -Werror -O2
    "${PROJECT_SOURCE_DIR}/src/base/c/neko_sha256.c"
    -o "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
  DEPENDS "${PROJECT_SOURCE_DIR}/src/base/c/neko_sha256.c" VERBATIM)
foreach(runner IN ITEMS runner lifecycle scheduler)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${runner}.js" "${CMAKE_CURRENT_BINARY_DIR}/${runner}.wasm"
    COMMAND "${NEKOMATA_EMXX}" ${wasm_flags} -sMAIN_MODULE=1 -sENVIRONMENT=web
      -sALLOW_MEMORY_GROWTH=1 -sFETCH=1
      "-I${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${runner}.cpp"
      "${wasm_backend}/candidate.cpp" "${wasm_backend}/emscripten_loader.cpp"
      "${wasm_backend}/emscripten_scheduler.cpp"
      "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
      -o "${CMAKE_CURRENT_BINARY_DIR}/${runner}.js"
    DEPENDS "${runner}.cpp" contract.hpp "${CMAKE_CURRENT_BINARY_DIR}/fixture_digests.h"
      "${wasm_backend}/candidate.cpp" "${wasm_backend}/candidate.hpp"
      "${wasm_backend}/emscripten_loader.cpp" "${wasm_backend}/emscripten_loader.hpp"
      "${wasm_backend}/emscripten_scheduler.cpp" "${wasm_backend}/emscripten_scheduler.hpp"
      "${wasm_backend}/poll_scheduler.hpp"
      "${wasm_backend}/module_descriptor.hpp"
      "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o" VERBATIM)
endforeach()
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fixture_digests.h"
  COMMAND "${CMAKE_COMMAND}"
    "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/fixture_digests.h"
    "-DA=${CMAKE_CURRENT_BINARY_DIR}/a.wasm"
    "-DB=${CMAKE_CURRENT_BINARY_DIR}/b.wasm"
    "-DINCOMPATIBLE=${CMAKE_CURRENT_BINARY_DIR}/incompatible.wasm"
    "-DINCOMPLETE=${CMAKE_CURRENT_BINARY_DIR}/incomplete.wasm"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_digests.cmake"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/a.wasm" "${CMAKE_CURRENT_BINARY_DIR}/b.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/incompatible.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/incomplete.wasm" VERBATIM)
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/late.wasm" "${CMAKE_CURRENT_BINARY_DIR}/abandoned.wasm"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/a.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/late.wasm"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/a.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/abandoned.wasm"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/a.wasm" VERBATIM)
configure_file(index.html index.html COPYONLY)
add_custom_target(neko_wasm_fixtures ALL DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/runner.js" "${CMAKE_CURRENT_BINARY_DIR}/runner.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/lifecycle.js" "${CMAKE_CURRENT_BINARY_DIR}/lifecycle.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/scheduler.js" "${CMAKE_CURRENT_BINARY_DIR}/scheduler.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/late.wasm" "${CMAKE_CURRENT_BINARY_DIR}/abandoned.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/a.wasm" "${CMAKE_CURRENT_BINARY_DIR}/b.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/incompatible.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/incomplete.wasm")
add_test(NAME neko.e2e.wasm.generations
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
    --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(neko.e2e.wasm.generations PROPERTIES TIMEOUT 60 LABELS "e2e;wasm")
add_test(NAME neko.e2e.wasm.candidate_lifetime
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
    --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner lifecycle)
set_tests_properties(neko.e2e.wasm.candidate_lifetime PROPERTIES TIMEOUT 60 LABELS "e2e;wasm")
add_test(NAME neko.e2e.wasm.poll_scheduler
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
    --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner scheduler)
set_tests_properties(neko.e2e.wasm.poll_scheduler PROPERTIES TIMEOUT 60 LABELS "e2e;wasm")
