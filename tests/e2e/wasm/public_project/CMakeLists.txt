cmake_minimum_required(VERSION 3.24)
project(neko_wasm_public_consumer LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)
include("${nekomata_DIR}/nekomata.cmake")

# The page receives registration from linked group targets, not handwritten C++.
set(GENERATION_ID 1 CACHE STRING "Fixture generation")
set(INTERFACE_VERSION 1 CACHE STRING "Fixture descriptor version")
nekomata_add_reload_unit(beta_hot SOURCES ../generation.cpp)
target_link_libraries(beta_hot PRIVATE nekomata::runtime)
target_compile_definitions(beta_hot PRIVATE
  GENERATION_ID=${GENERATION_ID} INTERFACE_VERSION=${INTERFACE_VERSION} MISSING_UPDATE=0)
nekomata_add_reload_group(beta UNITS beta_hot
  GROUP_ID beta ABI_ID flock-test-v1 ENTRIES identify update_world
  MANIFEST_URL beta/latest OFFER_ROOT "${FIXTURE_OUTPUT}/beta")
nekomata_add_reload_group(alpha SOURCES ../generation.cpp
  GROUP_ID alpha ABI_ID flock-test-v1 ENTRIES identify update_world
  MANIFEST_URL alpha/latest OFFER_ROOT "${FIXTURE_OUTPUT}/alpha"
  PLT_HEADER "plt.hpp")
target_link_libraries(alpha_neko_unit PRIVATE nekomata::runtime)
target_compile_definitions(alpha_neko_unit PRIVATE
  GENERATION_ID=${GENERATION_ID} INTERFACE_VERSION=${INTERFACE_VERSION} MISSING_UPDATE=0)
add_executable(public_session ../public_session.cpp)
target_link_libraries(public_session PRIVATE nekomata::neko beta alpha)
target_compile_options(public_session PRIVATE -Wall -Wextra -Werror)
target_link_options(public_session PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(public_session PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")

add_executable(public_empty ../public_empty.cpp)
target_link_libraries(public_empty PRIVATE nekomata::neko)
target_compile_options(public_empty PRIVATE -Wall -Wextra -Werror)
target_link_options(public_empty PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(public_empty PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")
