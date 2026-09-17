# PE binary backend (Windows).
#
# format/        offline COFF reading (relocations, sections) — planned
# runtime/       in-process hot replacement (code_pages)
# debug/         PDB symbol source through the MS DIA SDK — planned;
#                deliberately not a hand-rolled parser, with an interface
#                boundary left open for a future self-contained one
add_library(neko_backend_pe STATIC
  runtime/code_pages.cpp)
add_library(nekomata::backends::pe ALIAS neko_backend_pe)

target_include_directories(neko_backend_pe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(neko_backend_pe
  PUBLIC neko_runtime
  PRIVATE neko_base_c)

nekomata_apply_warnings(neko_backend_pe)
nekomata_apply_sanitizers(neko_backend_pe)
nekomata_apply_clang_tidy(neko_backend_pe)

# Symbol backend placeholder until the DIA reader lands.
add_library(neko_backend_pdb INTERFACE)
add_library(nekomata::backends::pdb ALIAS neko_backend_pdb)
