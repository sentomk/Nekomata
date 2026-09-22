cmake_minimum_required(VERSION 3.24)
project(neko_wasm_session_fixture LANGUAGES CXX)

set(neko_root "${CMAKE_CURRENT_LIST_DIR}/../../../..")
include("${neko_root}/cmake/nekomata.cmake")
set(GENERATION_ID "1" CACHE STRING "Fixture behavior identity")
set(INTERFACE_VERSION "1" CACHE STRING "Fixture descriptor version")
set(GROUP_ID "flock" CACHE STRING "Fixture reload group identity")

nekomata_add_reload_unit(flock_hot SOURCES ../generation.cpp)
target_include_directories(flock_hot PRIVATE "${neko_root}/src")
target_compile_definitions(flock_hot PRIVATE
  GENERATION_ID=${GENERATION_ID} INTERFACE_VERSION=${INTERFACE_VERSION} MISSING_UPDATE=0)
nekomata_add_reload_group(flock UNITS flock_hot
  GROUP_ID "${GROUP_ID}" ABI_ID "flock-test-v1" ENTRIES identify update_world
  OFFER_ROOT "${OFFER_ROOT}")
