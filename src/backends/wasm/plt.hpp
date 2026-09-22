#pragma once

// The wasm PLT: backend-owned indirection that keeps application call sites
// ordinary direct calls, the ELF/PE source shape. A side module's code is
// immutable once instantiated and undefined-symbol imports are fixed at
// instantiation, so the only re-pointable seam is an indirect call that
// exists when the main module compiles. Each reloadable entry gets one
// typed slot here plus a trampoline with the application-facing name; the
// backend rewrites the slots at activation, inside the caller's safe point.
//
// The generated registration TU includes an application header that defines
// the slots and trampolines (two plain lines per entry); the slot registers
// itself, so the backend learns name-to-slot mapping without generated code
// knowing signatures.

#include <string_view>
#include <utility>

#include "module_descriptor.hpp"

namespace neko::wasm::detail {

struct plt_slot_record {
  std::string_view group;
  std::string_view entry;
  module_function* target; // address of the owning slot's function pointer
  plt_slot_record* next = nullptr;
};

// Static-init registration, mirroring register_group: linked list, no
// allocation, no throwing.
void register_plt_slot(plt_slot_record& record) noexcept;

} // namespace neko::wasm::detail

namespace neko::wasm {

/// A static-duration function pointer the reload backend rewrites at
/// activation. `Signature` is the entry's function-pointer type. Construct
/// one per entry at namespace scope in the application's PLT header and
/// forward the trampoline through it.
template <typename Signature>
class plt_slot {
public:
  plt_slot(std::string_view group, std::string_view entry)
      : record_{group, entry, reinterpret_cast<module_function*>(&target)} {
    detail::register_plt_slot(record_);
  }

  plt_slot(const plt_slot&) = delete;
  plt_slot& operator=(const plt_slot&) = delete;

  /// The active generation's function; null before the first activation.
  Signature target = nullptr;

  /// Whether a generation has been activated into this slot yet.
  [[nodiscard]] explicit operator bool() const { return target != nullptr; }

  template <typename... Args>
  decltype(auto) operator()(Args&&... args) const {
    return target(std::forward<Args>(args)...);
  }

private:
  detail::plt_slot_record record_;
};

} // namespace neko::wasm
