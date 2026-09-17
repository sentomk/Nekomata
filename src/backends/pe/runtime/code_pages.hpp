// code_pages — executable memory and entry redirection on Windows.
//
// The rel32 constraint: a 5-byte `jmp rel32` reaches ±2 GiB. code_pages finds
// free address-space windows through VirtualQuery, reserves reusable
// PAGE_NOACCESS pools with MEM_RESERVE, and only commits slots whose complete
// usable span is reachable from the old function. A distant code region gets
// another pool; the number of translation units is not a probe-count limit.
//
// Reserve/commit separation replaces the ELF pool's explicit guard page:
// every slot commits only its usable span (plus one spare page), and the
// reservation around it stays uncommitted. Any write past the usable span —
// from any code path, in any geometry, in test runs and in user processes
// alike — faults deterministically instead of silently corrupting whatever is
// mapped next. Discarded candidates decommit their slot for reuse.
//
// Each reservation is represented by an owning executable_allocation. A
// discarded candidate returns its slot automatically; committed code can be
// transferred to process lifetime when its installed redirects outlive the
// reload session. commit_code additionally verifies the image fits its
// reservation, so overflow attempts are rejected loudly before any write.
//
// Entry patching safety at /Od (surveyed on MSVC 19.x, guarded at runtime):
//   * a shadow-space argument spill — `mov [rsp+08h..20h], reg` for one of
//     ecx/edx/r8d/r9d or rcx/rdx/r8/r9 — 4 or 5 bytes; the shadow store is
//     caller-owned scratch and the fresh code performs its own spill;
//   * or `sub rsp, imm8/imm32` for functions with no register arguments;
//   * or an entry WE patched before (E9 rel32 whose target lies inside one
//     of our own committed slots) — re-patching a previous redirect is how
//     repeated reloads of the same function work.
// The 5-byte write may overrun the first instruction by one byte; that byte
// belongs to the dead old body, and nothing branches into the first bytes of
// an /Od frame function. An ILT thunk also starts with E9, but its target is
// module code outside our slots, so incremental-link thunks are refused
// rather than confused with our redirects.
// All cases require the function body to be at least 5 bytes, checked by the
// caller (loader).

#pragma once

#include <cstdint>
#include <memory>

#include <neko/backend/code_substituter.hpp>

namespace neko::pe {

using neko::backend::code_substituter;

class code_pages final : public code_substituter {
public:
  code_pages();
  ~code_pages() override;

  backend::executable_allocation_ptr reserve_code_near(std::uintptr_t hint,
                                                       std::uint64_t bytes) override;
  backend::writable_allocation_ptr reserve_writable_near(std::uintptr_t hint,
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
  std::unique_ptr<allocation> reserve_near(std::uintptr_t hint, std::uint64_t bytes);
  std::shared_ptr<allocation_state> state_;
};

} // namespace neko::pe
