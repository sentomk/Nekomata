# Fixture consumer of the CMake adapter: one reload group, one executable,
# the documented integration shape from cmake-integration-design.md.
cmake_minimum_required(VERSION 3.21)
project(neko_cmake_adapter LANGUAGES CXX)

set(NEKOMATA_SOURCE_DIR "" CACHE PATH "nekomata checkout")

# Consume the Nekomata tree the way a find_package-less early adopter does;
# package export replaces this when the adapter ships installed.
set(NEKOMATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NEKOMATA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEKOMATA_TUI OFF CACHE BOOL "" FORCE)
set(NEKOMATA_ENABLE_DWARF_INSPECTION OFF CACHE BOOL "" FORCE)
add_subdirectory("${NEKOMATA_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/nekomata" EXCLUDE_FROM_ALL)

include("${NEKOMATA_SOURCE_DIR}/cmake/nekomata.cmake")

nekomata_add_reload_group(demo_hot SOURCES hot.cpp)

if(TARGET nekomata::backends::elf)
  add_executable(adapter_demo main.cpp)
  target_compile_options(adapter_demo PRIVATE -O0 -fno-pie -fno-pic)
  target_link_options(adapter_demo PRIVATE -no-pie)
  target_link_libraries(adapter_demo PRIVATE demo_hot nekomata::backends::elf)
else()
  # Without a runtime backend only the build-side publication is testable.
  message(STATUS "neko_cmake_adapter: no runtime backend; build-side only")
endif()
