# ELF binary backend (Linux).
#
# format/        offline file reading (binary_file, object_file)
# runtime/       in-process hot replacement (loader, process_symbols, code_pages)
# debug/         offline debug information (dwarf_inspect, function_index,
#                symbol_manifest)
add_library(neko_backend_elf STATIC
  elf.cpp
  format/object_file.cpp
  runtime/loader.cpp
  runtime/process_symbols.cpp
  runtime/code_pages.cpp
  debug/symbol_manifest.cpp)
add_library(nekomata::backends::elf ALIAS neko_backend_elf)

target_include_directories(neko_backend_elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(neko_backend_elf
  PUBLIC neko_runtime
  PRIVATE neko_base_c ${CMAKE_DL_LIBS})

# Public entry point is <neko/elf.hpp> (via nekomata::neko);
# internal headers include each other as "format/…", "runtime/…", "debug/…".

nekomata_apply_warnings(neko_backend_elf)
nekomata_apply_sanitizers(neko_backend_elf)
nekomata_apply_clang_tidy(neko_backend_elf)

# Offline file inspection is independent of the in-process backend and DWARF.
add_library(neko_elf_file STATIC format/binary_file.cpp)
nekomata_apply_warnings(neko_elf_file)
nekomata_apply_sanitizers(neko_elf_file)
nekomata_apply_clang_tidy(neko_elf_file)

# Offline DWARF inspection: no dependency from the runtime or public headers.
if(NOT NEKOMATA_ENABLE_DWARF_INSPECTION)
  return()
endif()

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Scope upstream's generic option names. For offline builds, supply
# FETCHCONTENT_SOURCE_DIR_LIBDWARF with an unpacked copy of this version.
function(nekomata_add_libdwarf)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED ON)
  set(BUILD_NON_SHARED OFF)
  set(BUILD_DWARFDUMP OFF)
  set(BUILD_DWARFGEN OFF)
  set(BUILD_DWARFEXAMPLE OFF)
  set(DO_TESTING OFF)
  set(ENABLE_DECOMPRESSION OFF)
  FetchContent_Declare(libdwarf
    URL https://codeload.github.com/davea42/libdwarf-code/tar.gz/refs/tags/v2.3.2
    URL_HASH SHA256=7be0e6e17847b503317ed11ebf28668e6300b198cae3ec36cfe9126767c3be27)
  FetchContent_MakeAvailable(libdwarf)
endfunction()
nekomata_add_libdwarf()

add_library(neko_backend_dwarf STATIC debug/dwarf_inspect.cpp)
add_library(nekomata::backends::dwarf ALIAS neko_backend_dwarf)
target_link_libraries(neko_backend_dwarf PRIVATE libdwarf::dwarf-shared neko_elf_file)
target_include_directories(neko_backend_dwarf PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}")
# Keep third-party header diagnostics outside Nekomata's warning/tidy policy.
# Use SYSTEM includes explicitly to retain compatibility with CMake 3.21.
target_include_directories(neko_backend_dwarf SYSTEM PRIVATE
  $<TARGET_PROPERTY:libdwarf::dwarf-shared,INTERFACE_INCLUDE_DIRECTORIES>)
nekomata_apply_warnings(neko_backend_dwarf)
nekomata_apply_sanitizers(neko_backend_dwarf)
nekomata_apply_clang_tidy(neko_backend_dwarf)
