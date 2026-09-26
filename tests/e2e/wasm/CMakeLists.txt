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
  -fno-exceptions -fno-rtti -fvisibility=hidden -sASSERTIONS=2 "-I${PROJECT_SOURCE_DIR}/src"
  "-I${PROJECT_SOURCE_DIR}/include")
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
    DEPENDS generation.cpp generation.hpp world.hpp
      "${PROJECT_SOURCE_DIR}/include/neko/detail/wasm_module_descriptor.hpp" VERBATIM)
endforeach()
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
  COMMAND "${NEKOMATA_EMCC}" -c -Wall -Wextra -Werror -O2
    "${PROJECT_SOURCE_DIR}/src/base/c/neko_sha256.c"
    -o "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
  DEPENDS "${PROJECT_SOURCE_DIR}/src/base/c/neko_sha256.c" VERBATIM)
set(session_sources "${wasm_backend}/offer_poller.cpp" "${wasm_backend}/session.cpp"
  "${wasm_backend}/registration.cpp"
  "${PROJECT_SOURCE_DIR}/src/protocol/wasm_offer.cpp" "${PROJECT_SOURCE_DIR}/src/core/log.cpp")
foreach(runner IN ITEMS runner lifecycle scheduler session multi_session)
  set(runner_sources "")
  set(runner_flags "")
  if(runner STREQUAL "session" OR runner STREQUAL "multi_session")
    set(runner_sources ${session_sources})
    # The offer codec throws on malformed input; the poller catches it.
    set(runner_flags -fexceptions -sDISABLE_EXCEPTION_CATCHING=0
      "-I${PROJECT_SOURCE_DIR}/include")
  endif()
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${runner}.js" "${CMAKE_CURRENT_BINARY_DIR}/${runner}.wasm"
    COMMAND "${NEKOMATA_EMXX}" ${wasm_flags} ${runner_flags} -sMAIN_MODULE=1 -sENVIRONMENT=web
      -sALLOW_MEMORY_GROWTH=1 -sFETCH=1
      "-I${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${runner}.cpp"
      "${wasm_backend}/candidate.cpp" "${wasm_backend}/emscripten_loader.cpp"
      "${wasm_backend}/emscripten_scheduler.cpp"
      ${runner_sources}
      "${CMAKE_CURRENT_BINARY_DIR}/neko_sha256.o"
      -o "${CMAKE_CURRENT_BINARY_DIR}/${runner}.js"
    DEPENDS "${runner}.cpp" contract.hpp world.hpp "${CMAKE_CURRENT_BINARY_DIR}/fixture_digests.h"
      "${wasm_backend}/candidate.cpp" "${wasm_backend}/candidate.hpp"
      "${wasm_backend}/emscripten_loader.cpp" "${wasm_backend}/emscripten_loader.hpp"
      "${wasm_backend}/emscripten_scheduler.cpp" "${wasm_backend}/emscripten_scheduler.hpp"
      "${wasm_backend}/poll_scheduler.hpp"
      ${runner_sources} "${wasm_backend}/session.hpp" "${wasm_backend}/offer_poller.hpp"
      "${PROJECT_SOURCE_DIR}/include/neko/wasm.hpp"
      "${PROJECT_SOURCE_DIR}/include/neko/backend/session_driver.hpp"
      "${wasm_backend}/session_error.hpp" "${PROJECT_SOURCE_DIR}/include/neko/session.hpp"
      "${PROJECT_SOURCE_DIR}/include/neko/fwd.hpp"
      "${wasm_backend}/manifest_fetcher.hpp" "${PROJECT_SOURCE_DIR}/src/protocol/wasm_offer.hpp"
      "${PROJECT_SOURCE_DIR}/src/protocol/scanner.hpp"
      "${PROJECT_SOURCE_DIR}/include/neko/detail/wasm_module_descriptor.hpp"
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
  "${CMAKE_CURRENT_BINARY_DIR}/session.js" "${CMAKE_CURRENT_BINARY_DIR}/session.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/multi_session.js" "${CMAKE_CURRENT_BINARY_DIR}/multi_session.wasm"
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

if(TARGET neko_publisher)
  find_program(NEKOMATA_EMCMAKE NAMES emcmake REQUIRED)
  get_filename_component(build_tool_dir "${CMAKE_MAKE_PROGRAM}" DIRECTORY)
  find_program(NEKOMATA_WASM_NINJA NAMES ninja HINTS "${build_tool_dir}" REQUIRED)
  file(GLOB_RECURSE public_library_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/neko/*" "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/*.hpp" "${PROJECT_SOURCE_DIR}/src/*.c"
    "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/CMakeLists.txt"
    "${PROJECT_SOURCE_DIR}/src/*/CMakeLists.txt" "${PROJECT_SOURCE_DIR}/cmake/*")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/public_session.js" "${CMAKE_CURRENT_BINARY_DIR}/public_session.wasm"
      "${CMAKE_CURRENT_BINARY_DIR}/public_release.js" "${CMAKE_CURRENT_BINARY_DIR}/public_release.wasm"
      "${CMAKE_CURRENT_BINARY_DIR}/public_empty.js" "${CMAKE_CURRENT_BINARY_DIR}/public_empty.wasm"
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${PROJECT_SOURCE_DIR}"
      "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}" "-DEMCMAKE=${NEKOMATA_EMCMAKE}"
      "-DNINJA=${NEKOMATA_WASM_NINJA}" "-DPUBLISHER=$<TARGET_FILE:neko_publisher>" -P "${CMAKE_CURRENT_SOURCE_DIR}/build_public.cmake"
    DEPENDS ${public_library_inputs} "${PROJECT_SOURCE_DIR}/CMakeLists.txt"
      build_public.cmake public_project/CMakeLists.txt public_project/plt.hpp public_session.cpp
      public_release.cpp public_empty.cpp world.hpp VERBATIM)
  add_custom_target(neko_wasm_public_fixture DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/public_session.js" "${CMAKE_CURRENT_BINARY_DIR}/public_session.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/public_release.js" "${CMAKE_CURRENT_BINARY_DIR}/public_release.wasm"
    "${CMAKE_CURRENT_BINARY_DIR}/public_empty.js" "${CMAKE_CURRENT_BINARY_DIR}/public_empty.wasm")
  add_dependencies(neko_wasm_public_fixture neko_publisher)
  add_dependencies(neko_wasm_fixtures neko_wasm_public_fixture)
  add_dependencies(neko_wasm_fixtures neko_publisher)
  add_test(NAME neko.e2e.wasm.session_lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
      --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner session
      --cmake "${CMAKE_COMMAND}" --emcmake "${NEKOMATA_EMCMAKE}"
      --ninja "${NEKOMATA_WASM_NINJA}" --publisher "$<TARGET_FILE:neko_publisher>")
  set_tests_properties(neko.e2e.wasm.session_lifecycle PROPERTIES TIMEOUT 180 LABELS "e2e;wasm")
  add_test(NAME neko.e2e.wasm.multi_group_session
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
      --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner multi_session
      --cmake "${CMAKE_COMMAND}" --emcmake "${NEKOMATA_EMCMAKE}"
      --ninja "${NEKOMATA_WASM_NINJA}" --publisher "$<TARGET_FILE:neko_publisher>")
  set_tests_properties(neko.e2e.wasm.multi_group_session PROPERTIES TIMEOUT 180 LABELS "e2e;wasm")
  add_test(NAME neko.e2e.wasm.public_factory
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
      --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner public_session
      --cmake "${CMAKE_COMMAND}" --emcmake "${NEKOMATA_EMCMAKE}"
      --ninja "${NEKOMATA_WASM_NINJA}" --publisher "$<TARGET_FILE:neko_publisher>")
  set_tests_properties(neko.e2e.wasm.public_factory PROPERTIES TIMEOUT 180 LABELS "e2e;wasm")
  add_test(NAME neko.e2e.wasm.public_release
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
      --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner public_release
      --cmake "${CMAKE_COMMAND}" --emcmake "${NEKOMATA_EMCMAKE}"
      --ninja "${NEKOMATA_WASM_NINJA}" --publisher "$<TARGET_FILE:neko_publisher>")
  set_tests_properties(neko.e2e.wasm.public_release PROPERTIES TIMEOUT 180 LABELS "e2e;wasm")
  add_test(NAME neko.e2e.wasm.empty_registry
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_browser.py"
      --browser "${NEKOMATA_WASM_BROWSER}" --root "${CMAKE_CURRENT_BINARY_DIR}" --runner public_empty)
  set_tests_properties(neko.e2e.wasm.empty_registry PROPERTIES TIMEOUT 60 LABELS "e2e;wasm")
endif()
