// code_pages — executable memory and entry redirection on Linux.
//
// The rel32 constraint: a 5-byte `jmp rel32` reaches ±2 GiB. Fresh code is
// therefore mmap'd as close to the functions it replaces as the kernel lets
// us (hinted allocation, distance-checked, retried in 128 MiB steps).
//
// Entry patching safety at -O0 (current assumptions, guarded at runtime):
//   * prologue `push rbp; mov rbp,rsp` (55 48 89 E5) — overwriting the
//     first 5 bytes splits `sub rsp, N`'s encoding, which is fine because
//     nothing branches into the first 8 bytes of an -O0 frame function
//     (loop labels appear after the prologue);
//   * or `endbr64; push rbp` (F3 0F 1E FA 55, -fcf-protection default) —
//     5 bytes land exactly on an instruction boundary;
//   * or an entry WE patched before (E9 rel32 whose target lies inside one
//     of our own arenas) — re-patching a previous redirect is how repeated
//     reloads of the same function work. A genuine -O0 prologue never
//     starts with a jmp, and nothing else jumps into our arenas.
// All cases require st_size >= 8, checked by the caller (loader).

#pragma once

#include <cstdint>
#include <vector>

#include <neko/runtime/code_substituter.hpp>

namespace neko::elf {

class code_pages final : public code_substituter {
public:
  ~code_pages() override;

  void* reserve_code_near(std::uintptr_t hint, std::uint64_t bytes) override;
  bool commit_code(void* reservation, const void* image, std::uint64_t bytes) override;
  bool patch_entry(std::uintptr_t entry, void* target) override;

private:
  bool owns_address(std::uintptr_t address) const;

  struct arena_range {
    std::uintptr_t begin;
    std::uintptr_t end;
  };
  /// Arenas allocated so far (never freed today). Registrations let
  /// patch_entry prove that an E9 at an entry is one of ours.
  std::vector<arena_range> arenas_;
};

} // namespace neko::elf
