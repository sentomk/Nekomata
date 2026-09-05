# DWARF symbol backend (Linux) — Phase 2 (basic parsing may start in Phase 1).
#
# Planned scope:
#   * function address/extents from DWARF + ELF .symtab
#   * type layouts for Phase 5
#   * (Phase 4) inline unit reconstruction from DW_TAG_inlined_subroutine
#
# Status: not implemented. Stub keeps the directory wired into the build.
add_library(neko_backend_dwarf INTERFACE)
add_library(nekomata::backends::dwarf ALIAS neko_backend_dwarf)
