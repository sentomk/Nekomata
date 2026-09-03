# ELF binary backend (Linux) — Phase 1.
#
# Scope (proposal §Detailed Design):
#   * runtime mini-link: apply relocations of freshly compiled .o files
#     against the live process's real addresses
#   * manage executable pages handed to the runtime agent (mmap/mprotect)
#
# Status: not implemented. This stub keeps the directory wired into the
# build so the structure is in place before the first real code lands.
add_library(neko_backend_elf INTERFACE)
add_library(nekomata::backends::elf ALIAS neko_backend_elf)
