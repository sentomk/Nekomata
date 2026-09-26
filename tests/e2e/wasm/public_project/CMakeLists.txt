cmake_minimum_required(VERSION 3.24)
project(neko_wasm_public_consumer LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)
include("${nekomata_DIR}/nekomata.cmake")

# The page receives registration from linked group targets, not handwritten C++.
# One group names its PLT header by absolute path and the other relatively,
# covering both path forms.
set(GENERATION_ID 1 CACHE STRING "Fixture generation")
set(INTERFACE_VERSION 1 CACHE STRING "Fixture compatibility version")
set(alpha_abi_id flock-test-v1)
if(INTERFACE_VERSION EQUAL 2)
  set(alpha_abi_id flock-test-incompatible)
endif()
nekomata_add_reload_unit(beta_hot SOURCES ../generation.cpp)
target_compile_definitions(beta_hot PRIVATE
  GENERATION_ID=${GENERATION_ID} NEKO_GENERATED_DESCRIPTOR=1)
nekomata_add_reload_group(beta UNITS beta_hot
  GROUP_ID beta ABI_ID flock-test-v1 ENTRIES identify update_world
  MANIFEST_URL beta/latest OFFER_ROOT "${FIXTURE_OUTPUT}/beta"
  EXPORT_HEADER "../generation.hpp" EXPORT_NAMESPACE wasm_fixture
  PLT_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/plt.hpp")
nekomata_add_reload_group(alpha SOURCES ../generation.cpp
  GROUP_ID alpha ABI_ID ${alpha_abi_id} ENTRIES identify update_world
  MANIFEST_URL alpha/latest OFFER_ROOT "${FIXTURE_OUTPUT}/alpha"
  EXPORT_HEADER "../generation.hpp" EXPORT_NAMESPACE wasm_fixture
  PLT_HEADER "plt.hpp")
target_compile_definitions(alpha_neko_unit PRIVATE
  GENERATION_ID=${GENERATION_ID} NEKO_GENERATED_DESCRIPTOR=1)
add_executable(public_session ../public_session.cpp)
target_link_libraries(public_session PRIVATE nekomata::neko beta alpha)
target_compile_options(public_session PRIVATE -Wall -Wextra -Werror)
target_link_options(public_session PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(public_session PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")

# An optimized page without assertions, as an ordinary release build links it.
add_executable(public_release ../public_release.cpp)
target_link_libraries(public_release PRIVATE nekomata::neko beta alpha)
target_compile_options(public_release PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(public_release PRIVATE -O2 -sENVIRONMENT=web -sASSERTIONS=0)
set_target_properties(public_release PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")

add_executable(public_empty ../public_empty.cpp)
target_link_libraries(public_empty PRIVATE nekomata::neko)
target_compile_options(public_empty PRIVATE -Wall -Wextra -Werror)
target_link_options(public_empty PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(public_empty PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")
