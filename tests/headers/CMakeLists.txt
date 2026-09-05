# Every public header must compile on its own, without an umbrella header or
# private include directories. Discover additions so new headers are checked too.
file(GLOB_RECURSE neko_public_headers CONFIGURE_DEPENDS
  RELATIVE "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/include/neko/*.hpp")
list(APPEND neko_public_headers "neko/core/version.hpp")

set(neko_header_sources fwd.cpp)
foreach(header IN LISTS neko_public_headers)
  string(REPLACE "/" "_" source_name "${header}")
  set(source "${CMAKE_CURRENT_BINARY_DIR}/${source_name}.cpp")
  file(GENERATE OUTPUT "${source}"
    CONTENT "#include <${header}>\n#include <${header}>\n")
  list(APPEND neko_header_sources "${source}")
endforeach()

add_library(neko_header_checks OBJECT ${neko_header_sources})
target_link_libraries(neko_header_checks PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_header_checks)
