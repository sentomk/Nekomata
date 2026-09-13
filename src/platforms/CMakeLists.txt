# nekomata backends.
#
# Backends carry all platform knowledge and are isolated in their own
# directories with their own tests:
#
#   backend            Linux             Windows
#   -----------------  ----------------  ------------------------------
#   compiler frontend  Clang (>= 14)     MSVC
#   symbol backend     DWARF + symtab    PDB (MS DIA SDK)
#   binary backend     ELF               PE
#   runtime agent      mmap/mprotect     VirtualProtect
#
# The kernel (include/neko) must stay buildable on every platform so the
# orchestration logic is testable everywhere; backends are gated below.

if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(elf)
  add_subdirectory(dwarf)

  # Joins the two offline readers at their common parent; the runtime does not
  # depend on this target.
  if(TARGET neko_backend_dwarf)
    add_library(neko_function_index STATIC elf/function_index.cpp)
    target_include_directories(neko_function_index
      PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(neko_function_index PRIVATE neko_elf_file neko_backend_dwarf)
    nekomata_apply_warnings(neko_function_index)
    nekomata_apply_sanitizers(neko_function_index)
    nekomata_apply_clang_tidy(neko_function_index)
  endif()
elseif(NEKOMATA_TARGET_WINDOWS)
  add_subdirectory(pe)
  add_subdirectory(pdb)
else()
  message(STATUS "nekomata: no backends for this platform; building kernel only")
endif()
