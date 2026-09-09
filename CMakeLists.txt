cmake_minimum_required(VERSION 3.21)

project(nekomata
  VERSION 0.2.0
  DESCRIPTION "Native hot-reload for C/C++ — no restarts, no refactor."
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NEKOMATA_BUILD_TESTS "Build nekomata tests" ON)
option(NEKOMATA_BUILD_TOOLS "Build nekomata command line tools" ON)
option(NEKOMATA_BUILD_EXAMPLES "Build nekomata examples" ON)
option(NEKOMATA_BUILD_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(NEKOMATA_TUI "Build the TUI module (requires Glyph, fetched)" OFF)
option(NEKOMATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NEKOMATA_ENABLE_SANITIZERS "Build with ASan+UBSan (Clang/GCC only)" OFF)
option(NEKOMATA_ENABLE_CLANG_TIDY "Run clang-tidy on nekomata code during builds" OFF)
option(NEKOMATA_ENABLE_DWARF_INSPECTION "Build the Linux offline DWARF inspector" ON)

if(NEKOMATA_ENABLE_CLANG_TIDY)
  find_program(NEKOMATA_CLANG_TIDY_EXECUTABLE NAMES clang-tidy-18 clang-tidy REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Public generated headers and compiled implementation.
# Runtime interfaces must not depend on platform backends.
# ---------------------------------------------------------------------------
configure_file(include/neko/version.hpp.in generated/neko/version.hpp @ONLY)

# ---------------------------------------------------------------------------
# Tests, tools & examples
# ---------------------------------------------------------------------------
# enable_testing() must run before any subdirectory that calls add_test().
if(NEKOMATA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(NEKOMATA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(NEKOMATA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(NEKOMATA_TUI)
  add_subdirectory(src/tui)
endif()

if(NEKOMATA_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "nekomata ${PROJECT_VERSION}")
message(STATUS "  compiler:   ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type: ${CMAKE_BUILD_TYPE}")
if(NEKOMATA_TARGET_LINUX)
  message(STATUS "  backends:   elf + dwarf (Linux)")
elseif(NEKOMATA_TARGET_WINDOWS)
  message(STATUS "  backends:   pe + pdb (Windows)")
else()
  message(STATUS "  backends:   none for this platform; building kernel only")
endif()
message(STATUS "  sanitizers: ${NEKOMATA_ENABLE_SANITIZERS}")
message(STATUS "  clang-tidy: ${NEKOMATA_ENABLE_CLANG_TIDY}")
message(STATUS "  tests:      ${NEKOMATA_BUILD_TESTS}")
