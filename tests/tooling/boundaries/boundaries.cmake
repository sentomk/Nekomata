# Architecture boundaries, asserted on the source tree and the build's
# compile database. These encode the module rules from the refactoring
# design: what may include what, and which compiler owns which layer.
# Script mode keeps the pre-3.3 if() grammar unless this policy is set.
cmake_policy(SET CMP0057 NEW)
set(errors "")

# Public headers never reach into the source tree, and only the backend
# extension headers expose backend assembly entry points.
file(GLOB_RECURSE public_headers "${SOURCE_DIR}/include/neko/*.hpp")
file(GLOB spi_headers "${SOURCE_DIR}/include/neko/backend.hpp"
  "${SOURCE_DIR}/include/neko/backend/*.hpp")
foreach(header IN LISTS public_headers)
  file(READ "${header}" text)
  if(text MATCHES "#include[^\\n]*(\"|<)[^\\n]*(src/|\\.\\./)")
    list(APPEND errors "${header}: public include reaches into the source tree")
  endif()
  if(text MATCHES "process_symbols|binary_file|code_pages|dwarf_inspect")
    list(APPEND errors "${header}: mentions a private backend type")
  endif()
  set(is_extension_header FALSE)
  if(header IN_LIST spi_headers)
    set(is_extension_header TRUE)
  endif()
  if(NOT is_extension_header)
    if(text MATCHES "make_handle")
      list(APPEND errors "${header}: exposes a backend assembly entry point; it belongs in <neko/backend.hpp>")
    endif()
  endif()
endforeach()

# Internal includes may nest inside a module's own subdirectories, but a
# traversal that escapes the module root is a cross-module dependency in
# disguise.
file(GLOB_RECURSE private_headers "${SOURCE_DIR}/src/*.hpp" "${SOURCE_DIR}/src/*.h")
foreach(header IN LISTS private_headers)
  file(READ "${header}" text)
  if(text MATCHES "#include[^\\n]*\\.\\./\\.\\./")
    list(APPEND errors "${header}: include escapes its module root")
  endif()
endforeach()

# The C base layer is compiled by the C compiler, not by accident.
if(EXISTS "${BUILD_DIR}/compile_commands.json")
  file(READ "${BUILD_DIR}/compile_commands.json" db)
  string(JSON entry_count LENGTH "${db}")
  set(sha_entry "")
  math(EXPR last "${entry_count} - 1")
  foreach(index RANGE "${last}")
    string(JSON entry GET "${db}" "${index}")
    string(JSON entry_file GET "${entry}" "file")
    if(entry_file MATCHES "neko_sha256\\.c$")
      set(sha_entry "${entry}")
      break()
    endif()
  endforeach()
  if("${sha_entry}" STREQUAL "")
    list(APPEND errors "compile_commands.json has no entry for neko_sha256.c")
  else()
    string(JSON entry_command GET "${sha_entry}" "command")
    if(entry_command MATCHES "-std=c\\+\\+")
      list(APPEND errors "neko_sha256.c is compiled as C++")
    endif()
  endif()
endif()

if(errors)
  foreach(error IN LISTS errors)
    message(STATUS "BOUNDARY VIOLATION: ${error}")
  endforeach()
  message(FATAL_ERROR "architecture boundary violations: ${errors}")
endif()
message(STATUS "architecture boundaries hold")
