# Reject decorative ruler comments in project-owned sources. Vendored sources
# and generated build trees retain their own style and are excluded.
set(errors "")
set(cpp_files "")
foreach(root IN ITEMS include src examples tests tools)
  file(GLOB_RECURSE found
    "${SOURCE_DIR}/${root}/*.c"
    "${SOURCE_DIR}/${root}/*.c.in"
    "${SOURCE_DIR}/${root}/*.cpp"
    "${SOURCE_DIR}/${root}/*.cpp.in"
    "${SOURCE_DIR}/${root}/*.h"
    "${SOURCE_DIR}/${root}/*.h.in"
    "${SOURCE_DIR}/${root}/*.hpp"
    "${SOURCE_DIR}/${root}/*.hpp.in")
  list(APPEND cpp_files ${found})
endforeach()
list(FILTER cpp_files EXCLUDE REGEX "/tests/support/doctest/")
list(FILTER cpp_files EXCLUDE REGEX "/build/")
list(REMOVE_DUPLICATES cpp_files)

foreach(path IN LISTS cpp_files)
  file(STRINGS "${path}" bad_lines REGEX "^[ \t]*//.*(----|====|——|────)")
  foreach(line IN LISTS bad_lines)
    list(APPEND errors "${path}: decorative comment: ${line}")
  endforeach()
endforeach()

set(script_files "${SOURCE_DIR}/CMakeLists.txt")
foreach(root IN ITEMS cmake examples scripts src tests tools)
  file(GLOB_RECURSE found
    "${SOURCE_DIR}/${root}/CMakeLists.txt"
    "${SOURCE_DIR}/${root}/*.cmake"
    "${SOURCE_DIR}/${root}/*.cmake.in"
    "${SOURCE_DIR}/${root}/*.sh"
    "${SOURCE_DIR}/${root}/*.sh.in")
  list(APPEND script_files ${found})
endforeach()
list(REMOVE_DUPLICATES script_files)
list(FILTER script_files EXCLUDE REGEX "/build/")

foreach(path IN LISTS script_files)
  file(STRINGS "${path}" bad_lines REGEX "^[ \t]*#[ \t]*.*(----|====|——|────)")
  foreach(line IN LISTS bad_lines)
    list(APPEND errors "${path}: decorative comment: ${line}")
  endforeach()
endforeach()

if(errors)
  foreach(error IN LISTS errors)
    message(STATUS "COMMENT STYLE VIOLATION: ${error}")
  endforeach()
  message(FATAL_ERROR "decorative ruler comments are not allowed")
endif()

message(STATUS "comment style holds")
