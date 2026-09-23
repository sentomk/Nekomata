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
#include <cstring>
#include <memory>

namespace neko::backend {

/// One executable-memory reservation. The handle exclusively owns the
/// mapping until it is either destroyed or explicitly transferred to process
/// lifetime after live function entries have been redirected into it.
class executable_allocation {
public:
  virtual ~executable_allocation() = default;

  [[nodiscard]] virtual void* data() noexcept = 0;
  [[nodiscard]] virtual const void* data() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

  /// Stop reclaiming this mapping with the handle. This is the final
  /// ownership transition for committed code whose redirects may outlive the
  /// reload_session that installed them.
  virtual void release_to_process() noexcept = 0;
};

using executable_allocation_ptr = std::unique_ptr<executable_allocation>;

/// One writable-memory reservation for mutable state introduced by a reload.
/// Its ownership follows the same candidate/commit/process-lifetime boundary
/// as executable code, but the mapping remains read+write for application use.
class writable_allocation {
public:
  virtual ~writable_allocation() = default;

  [[nodiscard]] virtual void* data() noexcept = 0;
  [[nodiscard]] virtual const void* data() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

  /// Stop reclaiming this mapping with the handle. Committed state may still
  /// be referenced by redirects that deliberately outlive reload_session.
  virtual void release_to_process() noexcept = 0;
};

using writable_allocation_ptr = std::unique_ptr<writable_allocation>;

class code_substituter {
public:
  virtual ~code_substituter() = default;

  /// Reserve writable memory for a fresh code image, placed so that a
  /// 5-byte `jmp rel32` from `hint` reaches it (within ±2 GiB). The
  /// returned handle owns the reservation; returns nullptr on failure.
  virtual executable_allocation_ptr reserve_code_near(std::uintptr_t hint, std::uint64_t bytes) = 0;

  /// Reserve read+write storage close enough to `hint` for the backend's
  /// direct data-reference encodings. The default preserves compatibility
  /// with backends that do not support globals introduced by fresh code.
  virtual writable_allocation_ptr reserve_writable_near(std::uintptr_t hint, std::uint64_t bytes) {
    (void)hint;
    (void)bytes;
    return nullptr;
  }

  /// Copy `bytes` from `image` into the reservation and flip it to
  /// read+execute. Invalidates instruction caches where required.
  virtual bool commit_code(executable_allocation& reservation, const void* image,
                           std::uint64_t bytes) = 0;

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

  /// Rewrite bytes inside a reservation after commit_code(), re-protecting
  /// the affected pages. Used to finish cross-object call targets once the
  /// whole candidate generation is loaded. Backends without page
  /// protections keep the plain-copy default.
  virtual bool rewrite_reservation(executable_allocation& reservation, std::uint64_t offset,
                                   const void* bytes, std::uint64_t size) {
    if (reservation.data() == nullptr || bytes == nullptr || offset > reservation.size() ||
        size > reservation.size() - offset) {
      return false;
    }
    std::memcpy(static_cast<std::uint8_t*>(reservation.data()) + offset, bytes,
                static_cast<std::size_t>(size));
    return true;
  }

  /// Attach a notification the backend runs when the reservation is
  /// reclaimed through its owning handle; `release_to_process` drops it
  /// silently, since released pages outlive every registration. Backends
  /// use this for process-wide registrations tied to an allocation's
  /// lifetime, such as unwind tables.
  virtual void on_reclaim(executable_allocation& reservation, void (*notify)(void*),
                          void* context) {
    static_cast<void>(reservation);
    static_cast<void>(notify);
    static_cast<void>(context);
  }
};

} // namespace neko::backend
