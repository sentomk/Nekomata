# The browser hot-reload demo as a real consumer of the CMake integration:
# one reload group under the Emscripten toolchain, one page target linking
# the private backend sources, publications landing in the served directory.
cmake_minimum_required(VERSION 3.24)
project(neko_wasm_reload_demo LANGUAGES C CXX)

set(neko_root "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${neko_root}/cmake/nekomata.cmake")

# Behavior selector for hot.cpp: switch with
#   cmake -B build -DDEMO_BEHAVIOR=2 && cmake --build build --target ball_reload
set(DEMO_BEHAVIOR "1" CACHE STRING "1=linear bounce, 2=gravity, 3=center spring")
if(NOT DEMO_BEHAVIOR MATCHES "^[123]$")
  message(FATAL_ERROR "DEMO_BEHAVIOR must be 1, 2, or 3")
endif()

set(public "${CMAKE_BINARY_DIR}/public")

# The persistent page: owns the world, drives the reload session.
add_executable(page
  main.cpp
  "${neko_root}/src/backends/wasm/candidate.cpp"
  "${neko_root}/src/backends/wasm/offer_poller.cpp"
  "${neko_root}/src/backends/wasm/session.cpp"
  "${neko_root}/src/backends/wasm/emscripten_loader.cpp"
  "${neko_root}/src/protocol/wasm_offer.cpp"
  "${neko_root}/src/base/c/neko_sha256.c")
target_include_directories(page PRIVATE "${neko_root}/src" "${CMAKE_CURRENT_LIST_DIR}")
# Exceptions stay enabled for the page: the offer codec reports malformed
# manifests by throwing. The side module keeps the fixture dialect.
target_compile_options(page PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-std=c++20 -fno-rtti>
  -O0 -g -Wall -Wextra -Werror)
target_link_options(page PRIVATE -sMAIN_MODULE=1 -sENVIRONMENT=web
  -sALLOW_MEMORY_GROWTH=1 -sFETCH=1 -sASSERTIONS=2)
set_target_properties(page PROPERTIES OUTPUT_NAME "main" RUNTIME_OUTPUT_DIRECTORY "${public}")

# The reloadable group: `ball_reload` republishes whenever hot.cpp changes.
nekomata_add_reload_unit(ball_hot SOURCES hot.cpp)
target_include_directories(ball_hot PRIVATE "${neko_root}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(ball_hot PRIVATE BEHAVIOR=${DEMO_BEHAVIOR})
nekomata_add_reload_group(ball UNITS ball_hot
  GROUP_ID "demo-ball"
  ABI_ID "demo-ball-v1"
  ENTRIES identify update_world
  OFFER_ROOT "${public}/offers")

add_custom_command(
  OUTPUT "${public}/index.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/index.html" "${public}/index.html"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/index.html" VERBATIM)
add_custom_target(demo_page ALL DEPENDS page "${public}/index.html")
