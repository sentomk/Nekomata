// code_pages — executable memory and entry redirection on Linux.
//
// The rel32 constraint: a 5-byte `jmp rel32` reaches ±2 GiB. code_pages
// finds real gaps in /proc/self/maps, reserves reusable PROT_NONE pools with
// MAP_FIXED_NOREPLACE, and only suballocates slots whose complete usable span
// is reachable from the old function. A distant code region gets another
// pool; the number of translation units is not a probe-count limit.
//
// Every allocation slot is followed by a PROT_NONE guard page. Any write past
// the reserved span — from any code path, in any geometry, in test runs and
// in user processes alike — turns into a deterministic SIGSEGV instead of
// silent corruption of whatever happens to be mapped next. Discarded
// candidates return their protected slot to the pool for reuse.
//
// Each reservation is represented by an owning executable_allocation. A
// discarded candidate returns its slot automatically; committed code can be
// transferred to process lifetime when its installed redirects outlive the
// reload session. commit_code additionally verifies the image fits its
// reservation, so overflow attempts are rejected loudly before any write.
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
#include <memory>

#include <neko/backend/code_substituter.hpp>

namespace neko::elf {

using neko::backend::code_substituter;

class code_pages final : public code_substituter {
public:
  code_pages();
  ~code_pages() override;

  backend::executable_allocation_ptr reserve_code_near(std::uintptr_t hint,
                                                       std::uint64_t bytes) override;
  bool commit_code(backend::executable_allocation& reservation, const void* image,
                   std::uint64_t bytes) override;
  bool precheck_entry(std::uintptr_t entry, void* target) override;
  bool snapshot_entry(std::uintptr_t entry, std::uint8_t out[5]) override;
  bool patch_entry(std::uintptr_t entry, void* target) override;
  bool rewrite_reservation(backend::executable_allocation& reservation, std::uint64_t offset,
                           const void* bytes, std::uint64_t size) override;
  bool restore_entry(std::uintptr_t entry, const std::uint8_t original[5]) override;

private:
  bool patchable_entry(std::uintptr_t entry, void* target) const;
  bool owns_address(std::uintptr_t address) const;

  struct allocation_state;
  class allocation;
  std::shared_ptr<allocation_state> state_;
};

} // namespace neko::elf
