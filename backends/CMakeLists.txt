# nekomata backends.
#
# Backends carry all platform knowledge and are isolated in their own
# directories with their own tests (proposal §Detailed Design):
#
#   backend            Linux             Windows
#   -----------------  ----------------  ------------------------------
#   compiler frontend  Clang (>= 14)     MSVC
#   symbol backend     DWARF + symtab    PDB (MS DIA SDK)
#   binary backend     ELF               PE
#   runtime agent      mmap/mprotect     VirtualProtect
#
# The kernel (include/hlr) must stay buildable on every platform so the
# orchestration logic is testable everywhere; backends are gated below.

if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(elf)
  add_subdirectory(dwarf)
elseif(NEKOMATA_TARGET_WINDOWS)
  add_subdirectory(pe)
  add_subdirectory(pdb)
else()
  message(STATUS "nekomata: no backends for this platform; building kernel only")
endif()
