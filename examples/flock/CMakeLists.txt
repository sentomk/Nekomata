# A browser page that consumes an installed Nekomata package. Configure with
# the Emscripten toolchain and NEKOMATA_PUBLISHER_EXECUTABLE pointing at a
# native publisher; run.sh does all of this.
cmake_minimum_required(VERSION 3.24)
project(nekomata_flock LANGUAGES C CXX)

find_package(nekomata CONFIG REQUIRED)
include("${nekomata_DIR}/nekomata.cmake")

set(public "${CMAKE_BINARY_DIR}/public")

# The ABI identity is a hash of the persistent contract. Editing either file
# reconfigures on the next build, so a generation built against a changed
# layout or signature carries a new identity that the running page rejects.
set(contract_files "${CMAKE_CURRENT_SOURCE_DIR}/src/world.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/flock.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${contract_files})
set(contract_text "")
foreach(file IN LISTS contract_files)
  file(READ "${file}" text)
  string(APPEND contract_text "${text}")
endforeach()
string(SHA256 contract_digest "${contract_text}")
string(SUBSTRING "${contract_digest}" 0 16 contract_digest)
set(flock_abi_id "flock-${contract_digest}")

nekomata_add_reload_group(flock SOURCES src/flock.cpp
  GROUP_ID flock
  ABI_ID "${flock_abi_id}"
  ENTRIES step describe
  EXPORT_HEADER src/flock.hpp
  EXPORT_NAMESPACE flock
  PLT_HEADER src/plt.hpp
  MANIFEST_URL offers/latest
  OFFER_ROOT "${public}/offers")

# The persistent page. It links only the group's registration; behavior
# arrives as published side modules.
add_executable(page src/main.cpp src/plt.cpp)
target_link_libraries(page PRIVATE nekomata::neko flock)
target_compile_options(page PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(page PRIVATE -O2 -sENVIRONMENT=web)
set_target_properties(page PROPERTIES OUTPUT_NAME main RUNTIME_OUTPUT_DIRECTORY "${public}")

add_custom_command(
  OUTPUT "${public}/index.html"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" "${public}/index.html"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" VERBATIM)
add_custom_target(site ALL DEPENDS page "${public}/index.html")
