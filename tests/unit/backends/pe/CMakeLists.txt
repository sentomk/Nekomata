# PE backend unit suites: allocation and entry-patch policy against real
# VirtualAlloc memory; no object fixtures yet (the COFF reader is planned).

set(neko_unit_include_dirs
  "${PROJECT_SOURCE_DIR}/src/backends/pe/runtime")

nekomata_add_unit_test(
  NAME unit.backends.pe.code_pages
  SOURCES code_pages.cpp
  PRIVATE_INCLUDES ${neko_unit_include_dirs}
  LIBRARIES neko_backend_pe)
