# ELF binary backend (Linux) — single-TU hot reload prototype.
#
# object_file     minimal ELF64 relocatable reader (sections/symbols/rela)
# process_symbols /proc/self/exe .symtab + dlsym; symbol_provider+state_manager
# code_pages      mmap arena near old code, entry patching (code_substituter)
# symbol_manifest the offline symbol-to-source map (disambiguation)
# loader          the runtime mini-link (object_loader)
add_library(neko_backend_elf STATIC
  object_file.cpp
  process_symbols.cpp
  symbol_manifest.cpp
  code_pages.cpp
  loader.cpp
  elf.cpp)
add_library(nekomata::backends::elf ALIAS neko_backend_elf)

target_link_libraries(neko_backend_elf
  PUBLIC nekomata::neko
  PRIVATE ${CMAKE_DL_LIBS})

# Public entry point is <neko/platforms/elf.hpp> (via nekomata::neko);
# internal headers use same-directory relative includes.

nekomata_apply_warnings(neko_backend_elf)
nekomata_apply_sanitizers(neko_backend_elf)
nekomata_apply_clang_tidy(neko_backend_elf)

# Offline file inspection is independent of the in-process backend and DWARF.
add_library(neko_elf_file STATIC binary_file.cpp)
nekomata_apply_warnings(neko_elf_file)
nekomata_apply_sanitizers(neko_elf_file)
nekomata_apply_clang_tidy(neko_elf_file)
