// code_substituter — executable memory and entry redirection.
//
// Owns the executable-page playground: reserving code pages close enough to
// existing code that a 5-byte `jmp rel32` reaches (±2 GiB), placing a
// relocated image into them, and rewriting function entry points.
//
// Backends: mmap/mprotect (POSIX, Phase 1), VirtualProtect (Windows, Phase 3).
//
// Phase 1 validation: the draft's reserve_code(bytes) could not know WHERE
// to allocate — rel32 reachability requires a hint — so it became
// reserve_code_near(hint, bytes). The draft's relocate() moved out entirely:
// relocations belong to the object_loader, which has the symbol context.
// See docs/phase-1-notes.md.

#pragma once

#include <cstdint>

namespace neko {

class code_substituter {
public:
  virtual ~code_substituter() = default;

  /// Reserve writable memory for a fresh code image, placed so that a
  /// 5-byte `jmp rel32` from `hint` reaches it (within ±2 GiB). Returns
  /// nullptr on failure.
  virtual void* reserve_code_near(std::uintptr_t hint, std::uint64_t bytes) = 0;

  /// Copy `bytes` from `image` into the reservation and flip it to
  /// read+execute. Invalidates instruction caches where required.
  virtual bool commit_code(void* reservation, const void* image, std::uint64_t bytes) = 0;

  /// Rewrite the function entry at `entry` so the next call lands in
  /// `target`. Returns false if the entry cannot be patched safely.
  virtual bool patch_entry(std::uintptr_t entry, void* target) = 0;
};

} // namespace neko
