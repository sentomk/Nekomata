# The browser demo consumes the installed library and CMake integration.
cmake_minimum_required(VERSION 3.24)
project(neko_wasm_reload_demo LANGUAGES C CXX)

find_package(nekomata CONFIG REQUIRED)
include("${nekomata_DIR}/nekomata.cmake")

# Behavior selector for hot.cpp: switch with
#   cmake -B build -DDEMO_BEHAVIOR=2 && cmake --build build --target ball_reload
set(DEMO_BEHAVIOR "1" CACHE STRING "1=linear bounce, 2=gravity, 3=center spring")
if(NOT DEMO_BEHAVIOR MATCHES "^[123]$")
  message(FATAL_ERROR "DEMO_BEHAVIOR must be 1, 2, or 3")
endif()

set(public "${CMAKE_BINARY_DIR}/public")

# The persistent page: owns the world, drives the public reload session.
add_executable(page main.cpp)
target_include_directories(page PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
# The installed target supplies the browser runtime and its required link flags.
target_compile_options(page PRIVATE -O0 -g -Wall -Wextra -Werror)
target_link_options(page PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(page PROPERTIES OUTPUT_NAME "main" RUNTIME_OUTPUT_DIRECTORY "${public}")

# The reloadable group: `ball_reload` republishes whenever hot.cpp changes.
nekomata_add_reload_unit(ball_hot SOURCES hot.cpp)
target_link_libraries(ball_hot PRIVATE nekomata::runtime)
target_include_directories(ball_hot PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(ball_hot PRIVATE BEHAVIOR=${DEMO_BEHAVIOR})
nekomata_add_reload_group(ball UNITS ball_hot
  GROUP_ID "demo-ball"
  ABI_ID "demo-ball-v1"
  MANIFEST_URL "offers/latest"
  ENTRIES identify update_world
  PLT_HEADER "plt.hpp"
  OFFER_ROOT "${public}/offers")
target_link_libraries(page PRIVATE nekomata::neko ball)

add_custom_command(
  OUTPUT "${public}/index.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/index.html" "${public}/index.html"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/index.html" VERBATIM)
add_custom_target(demo_page ALL DEPENDS page "${public}/index.html")
