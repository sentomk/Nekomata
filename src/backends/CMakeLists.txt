# nekomata backends.
#
# Backends carry all platform knowledge and are isolated in their own
# directories with their own tests. Their responsibilities map as follows:
#
# - compiler frontend: Clang (>= 14) on Linux; MSVC on Windows
# - symbol backend: DWARF + symtab on Linux; PDB (MS DIA SDK) on Windows
# - binary backend: ELF on Linux; PE on Windows
# - runtime agent: mmap/mprotect on Linux; VirtualProtect on Windows
#
# The kernel (include/neko) must stay buildable on every platform so the
# orchestration logic is testable everywhere. Backend runtimes stay gated
# below; the one exception is the COFF reader, whose offline byte parsing is
# platform-neutral — it builds everywhere so its suites run on hosts whose
# compilers can cross-produce fixtures.

if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(elf)

  # Joins the two offline readers at their common parent; the runtime does not
  # depend on this target.
  if(TARGET neko_backend_dwarf)
    add_library(neko_function_index STATIC elf/debug/function_index.cpp)
    target_include_directories(neko_function_index
      PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/elf")
    target_link_libraries(neko_function_index PRIVATE neko_elf_file neko_backend_dwarf)
    nekomata_apply_warnings(neko_function_index)
    nekomata_apply_sanitizers(neko_function_index)
    nekomata_apply_clang_tidy(neko_function_index)
  endif()
endif()

# The Windows runtime half gates itself inside pe/.
add_subdirectory(pe)
