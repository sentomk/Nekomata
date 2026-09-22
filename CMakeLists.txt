cmake_minimum_required(VERSION 3.21)

project(nekomata
  VERSION 0.2.0
  DESCRIPTION "Native hot-reload for C/C++ — no restarts, no refactor."
  LANGUAGES C CXX)

# Build options.
option(NEKOMATA_BUILD_TESTS "Build nekomata tests" ON)
option(NEKOMATA_BUILD_TOOLS "Build nekomata command line tools" ON)
option(NEKOMATA_BUILD_EXAMPLES "Build nekomata examples" ON)
option(NEKOMATA_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(NEKOMATA_TEST_WASM "Build browser WASM fixtures (requires em++ and Chromium)" OFF)
option(NEKOMATA_TUI "Build the TUI module (requires Glyph, fetched)" OFF)
option(NEKOMATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NEKOMATA_ENABLE_SANITIZERS "Build with ASan+UBSan (Clang/GCC only)" OFF)
option(NEKOMATA_ENABLE_CLANG_TIDY "Run clang-tidy on nekomata code during builds" OFF)
option(NEKOMATA_ENABLE_DWARF_INSPECTION "Build the Linux offline DWARF inspector" ON)

if(NEKOMATA_ENABLE_CLANG_TIDY)
  find_program(NEKOMATA_CLANG_TIDY_EXECUTABLE NAMES clang-tidy-18 clang-tidy REQUIRED)
endif()

# Global compile settings.
# Source and output are UTF-8: the cat faces in `cats.hpp` and the TUI's box
# drawing are multi-byte, and log lines are meant to reach the terminal as
# UTF-8. MSVC defaults both character sets to the machine's code page, which
# makes every `\u...` in a string literal C4566 — an error under /WX — and
# would otherwise mangle the output on the way out.
if(MSVC)
  add_compile_options(/utf-8)
endif()

# Include dirs for installation must be known before src/ references them.
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE
    PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Platform gates for the pluggable backends.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NEKOMATA_TARGET_LINUX ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(NEKOMATA_TARGET_WINDOWS ON)
endif()

# Shared CMake helpers.
function(nekomata_apply_clang_tidy target)
  if(NEKOMATA_ENABLE_CLANG_TIDY)
    set(tidy_command "${NEKOMATA_CLANG_TIDY_EXECUTABLE}"
      "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
    set_target_properties(${target} PROPERTIES
      C_CLANG_TIDY "${tidy_command}"
      CXX_CLANG_TIDY "${tidy_command}")
  endif()
endfunction()

function(nekomata_apply_warnings target)
  if(MSVC)
    set(flags /W4 /permissive-)
    if(NEKOMATA_WARNINGS_AS_ERRORS)
      list(APPEND flags /WX)
    endif()
  else()
    set(flags -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(NEKOMATA_WARNINGS_AS_ERRORS)
      list(APPEND flags -Werror)
    endif()
  endif()
  target_compile_options(${target} PRIVATE ${flags})
endfunction()

function(nekomata_apply_sanitizers target)
  if(NOT NEKOMATA_ENABLE_SANITIZERS)
    return()
  endif()
  # Instrument the target's own TUs (PRIVATE), and make consumers link the
  # runtime (INTERFACE) without instrumenting THEIR code. Full-project
  # instrumentation would shift global layouts (ASan redzones) and break the
  # reload demo's state anchoring.
  if(MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} INTERFACE /fsanitize=address)
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                                            -fno-sanitize-recover=all
                                            -fno-omit-frame-pointer)
    target_link_options(${target} INTERFACE -fsanitize=address,undefined)
  endif()
endfunction()

# Public generated headers and compiled implementation.
# Runtime interfaces must not depend on platform backends.
configure_file(include/neko/version.hpp.in generated/neko/version.hpp @ONLY)

# The generated config header describes what this build carries; the umbrella
# includes platform backends by platform and the TUI by this macro.
set(ENABLE_NEKOMATA_TUI 0)
if(NEKOMATA_TUI)
  set(ENABLE_NEKOMATA_TUI 1)
endif()
configure_file(include/neko/config.hpp.in generated/neko/config.hpp)

# Tests, tools, and examples.
# enable_testing() must run before any subdirectory that calls add_test().
if(NEKOMATA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

# The convenience aggregate: everything a default consumer needs. The ELF
# backend (added inside src/) and the TUI (added below) join as they exist.
add_library(neko_runtime_aggregate INTERFACE)
target_link_libraries(neko_runtime_aggregate INTERFACE neko_runtime)
if(EMSCRIPTEN)
  target_link_libraries(neko_runtime_aggregate INTERFACE neko_wasm_candidate)
endif()
if(TARGET neko_backend_elf)
  target_link_libraries(neko_runtime_aggregate INTERFACE neko_backend_elf)
endif()
if(TARGET neko_backend_pe)
  target_link_libraries(neko_runtime_aggregate INTERFACE neko_backend_pe)
endif()
add_library(nekomata::neko ALIAS neko_runtime_aggregate)

if(NEKOMATA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(NEKOMATA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(NEKOMATA_TUI)
  add_subdirectory(src/tui)
  if(TARGET neko_tui)
    target_link_libraries(neko_runtime_aggregate INTERFACE neko_tui)
  endif()
endif()

if(NEKOMATA_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Install and export the public targets and package metadata.
# The installed package exposes `nekomata::neko` (runtime + the platform
# backend), `nekomata::runtime`, `nekomata::elf`, and the adapter-facing
# `nekomata::host_publisher` executable. `neko_protocol` and `neko_base_c`
# ride the export for the static link closure; they are internal. The TUI
# is not exported yet: its Glyph dependency needs an install story first.
include(CMakePackageConfigHelpers)

if(NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
  target_link_libraries(neko_runtime PUBLIC Threads::Threads)
endif()

install(DIRECTORY include/neko
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")
install(FILES
  "${PROJECT_BINARY_DIR}/generated/neko/version.hpp"
  "${PROJECT_BINARY_DIR}/generated/neko/config.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/neko)

set(neko_export_targets neko_runtime neko_protocol neko_base_c)
if(EMSCRIPTEN)
  list(APPEND neko_export_targets neko_wasm_candidate)
  set_target_properties(neko_wasm_candidate PROPERTIES EXPORT_NAME wasm)
endif()
if(TARGET neko_backend_elf)
  list(APPEND neko_export_targets neko_backend_elf)
endif()
if(TARGET neko_backend_pe)
  # neko_pe_file rides along: the exported backend's static closure needs it.
  list(APPEND neko_export_targets neko_backend_pe neko_pe_file)
endif()
if(TARGET neko_publisher)
  list(APPEND neko_export_targets neko_publisher)
endif()
if(NOT NEKOMATA_TUI OR NOT TARGET neko_tui)
  # The aggregate is exportable only while every target it names is.
  list(APPEND neko_export_targets neko_runtime_aggregate)
  set_target_properties(neko_runtime_aggregate PROPERTIES EXPORT_NAME neko)
endif()

set_target_properties(neko_runtime PROPERTIES EXPORT_NAME runtime)
set_target_properties(neko_protocol PROPERTIES EXPORT_NAME protocol)
set_target_properties(neko_base_c PROPERTIES EXPORT_NAME base_c)
if(TARGET neko_backend_elf)
  set_target_properties(neko_backend_elf PROPERTIES EXPORT_NAME elf)
endif()
if(TARGET neko_backend_pe)
  set_target_properties(neko_backend_pe PROPERTIES EXPORT_NAME pe)
  set_target_properties(neko_pe_file PROPERTIES EXPORT_NAME pe_file)
endif()
if(TARGET neko_publisher)
  set_target_properties(neko_publisher PROPERTIES EXPORT_NAME host_publisher)
endif()

install(TARGETS ${neko_export_targets}
  EXPORT nekomata-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(EXPORT nekomata-targets
  NAMESPACE nekomata::
  FILE nekomata-targets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nekomata)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/nekomata-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
configure_file(cmake/nekomata-config.cmake.in
  "${PROJECT_BINARY_DIR}/nekomata-config.cmake" @ONLY)
install(FILES
  "${PROJECT_BINARY_DIR}/nekomata-config.cmake"
  "${PROJECT_BINARY_DIR}/nekomata-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nekomata)
# The build-adapter module; consumers include() it after find_package.
install(FILES cmake/nekomata.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nekomata)

# Configuration summary.
message(STATUS "nekomata ${PROJECT_VERSION}")
message(STATUS "  compiler:   ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type: ${CMAKE_BUILD_TYPE}")
if(EMSCRIPTEN)
  message(STATUS "  backends:   browser WASM")
elseif(NEKOMATA_TARGET_LINUX)
  message(STATUS "  backends:   elf + dwarf (Linux)")
elseif(NEKOMATA_TARGET_WINDOWS)
  message(STATUS "  backends:   pe + pdb (Windows)")
else()
  message(STATUS "  backends:   none for this platform; kernel plus the COFF reader")
endif()
message(STATUS "  sanitizers: ${NEKOMATA_ENABLE_SANITIZERS}")
message(STATUS "  clang-tidy: ${NEKOMATA_ENABLE_CLANG_TIDY}")
message(STATUS "  tests:      ${NEKOMATA_BUILD_TESTS}")
