# Architecture boundaries, asserted on the source tree and the build's
# compile database. These encode the module rules from the refactoring
# design: what may include what, and which compiler owns which layer.
set(errors "")

# Public headers never reach into the source tree.
file(GLOB_RECURSE public_headers "${SOURCE_DIR}/include/neko/*.hpp")
foreach(header IN LISTS public_headers)
  file(READ "${header}" text)
  if(text MATCHES "#include[^\\n]*(\"|<)[^\\n]*(src/|\\.\\./)")
    list(APPEND errors "${header}: public include reaches into the source tree")
  endif()
  if(text MATCHES "process_symbols|binary_file|code_pages|dwarf_inspect")
    list(APPEND errors "${header}: mentions a private backend type")
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
  string(REGEX MATCH "\\{[^\\{]*neko_sha256\\.c[^\\{]*\\}" entry "${db}")
  if("${entry}" STREQUAL "")
    list(APPEND errors "compile_commands.json has no entry for neko_sha256.c")
  elseif(entry MATCHES "-std=c\\+\\+")
    list(APPEND errors "neko_sha256.c is compiled as C++")
  endif()
endif()

if(errors)
  foreach(error IN LISTS errors)
    message(STATUS "BOUNDARY VIOLATION: ${error}")
  endforeach()
  message(FATAL_ERROR "architecture boundary violations: ${errors}")
endif()
message(STATUS "architecture boundaries hold")
