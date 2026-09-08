// code_substituter — executable memory and entry redirection.
//
// Owns the executable-page playground: reserving code pages close enough to
// existing code that a 5-byte `jmp rel32` reaches (±2 GiB), placing a
// relocated image into them, and rewriting function entry points.
//
// Entry patching is a two-phase protocol driven by the session:
//   precheck_entry  zero-write feasibility check (prologue pattern,
//                   rel32 reach, page access)
//   snapshot_entry  read the current 5 bytes at an entry
//   patch_entry     write the redirect (only sane after a successful
//                   precheck of the same entry)
//   restore_entry   write saved bytes back (rollback path)
// This is what makes multi-function reloads atomic: precheck everything,
// snapshot everything, patch everything, roll back on any failure.
//
// Backends: mmap/mprotect (POSIX, today), VirtualProtect (Windows, planned).

#pragma once

#include <cstdint>

#include <neko/runtime/fwd.hpp>

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

  /// Zero-write feasibility check for patching `entry` to jump to `target`:
  /// known prologue (or one of our own redirects), rel32 reach, page access.
  virtual bool precheck_entry(std::uintptr_t entry, void* target) = 0;

  /// Read the current 5 bytes at `entry` into `out` (for rollback).
  virtual bool snapshot_entry(std::uintptr_t entry, std::uint8_t out[5]) = 0;

  /// Rewrite the function entry at `entry` so the next call lands in
  /// `target`. Returns false if the entry cannot be patched safely.
  virtual bool patch_entry(std::uintptr_t entry, void* target) = 0;

  /// Write 5 saved bytes back to `entry` and restore execute-only pages.
  virtual bool restore_entry(std::uintptr_t entry, const std::uint8_t original[5]) = 0;
};

} // namespace neko
