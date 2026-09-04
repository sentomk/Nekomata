// code_pages — executable memory and entry redirection on Linux.
//
// The rel32 constraint: a 5-byte `jmp rel32` reaches ±2 GiB. Fresh code is
// therefore mmap'd as close to the functions it replaces as the kernel lets
// us (hinted allocation, distance-checked, retried in 128 MiB steps).
//
// Entry patching safety at -O0 (Phase 1 assumptions, guarded at runtime):
//   * prologue `push rbp; mov rbp,rsp` (55 48 89 E5) — overwriting the
//     first 5 bytes splits `sub rsp, N`'s encoding, which is fine because
//     nothing branches into the first 8 bytes of an -O0 frame function
//     (loop labels appear after the prologue);
//   * or `endbr64; push rbp` (F3 0F 1E FA 55, -fcf-protection default) —
//     5 bytes land exactly on an instruction boundary.
// Both patterns require st_size >= 8, checked by the caller (loader).

#pragma once

#include <cstdint>

#include <neko/code_substituter.hpp>

namespace neko::elf {

class code_pages final : public code_substituter {
public:
    ~code_pages() override;

    void* reserve_code_near(std::uintptr_t hint, std::uint64_t bytes) override;
    bool commit_code(void* reservation, const void* image, std::uint64_t bytes) override;
    bool patch_entry(std::uintptr_t entry, void* target) override;
};

} // namespace neko::elf
