#pragma once

#include <neko/session.hpp>

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace neko::wasm::detail {

using plt_function = void (*)();

// Type-erased view of one plt_slot. `assign` converts the generic entry back
// to the slot's own pointer type, so the slot is never written through an
// lvalue of a different function pointer type.
struct plt_slot_record {
  std::string_view group;
  std::string_view entry;
  void* slot;
  void (*assign)(void* slot, plt_function target) noexcept;
  plt_slot_record* next = nullptr;
};

void register_plt_slot(plt_slot_record& record) noexcept;

} // namespace neko::wasm::detail

namespace neko::wasm {
#if defined(__EMSCRIPTEN__)
/// Discover build-generated groups, initially disabled, and own their browser
/// transport, scheduling and preparation. Pass the result to reload_session.
/// Only one browser backend may exist per page because PLT slots are global.
[[nodiscard]] backend_handle create_backend();
#endif

/// A static-duration function pointer rewritten when a generation activates.
/// Define one slot and forwarding function per entry in the application header
/// passed as PLT_HEADER to nekomata_add_reload_group. Call only after the
/// group's first generation has activated.
///
/// `Signature` is a function type or a pointer to one. Prefer naming the
/// side-module declaration, as in `plt_slot<decltype(game::tick)>`: the
/// operand is unevaluated, so the main module needs no definition, and a
/// signature change in the export header then fails to compile here instead
/// of trapping at an indirect call. Each group and entry must be registered
/// by the build adapter; the backend rejects unknown slots before observing.
template <typename Signature>
class plt_slot {
public:
  using pointer =
      std::conditional_t<std::is_function_v<Signature>, std::add_pointer_t<Signature>, Signature>;
  static_assert(std::is_pointer_v<pointer> && std::is_function_v<std::remove_pointer_t<pointer>>,
                "plt_slot requires a function type or a function pointer type");

  plt_slot(std::string_view group, std::string_view entry)
      : record_{group, entry, this, &plt_slot::assign} {
    detail::register_plt_slot(record_);
  }

  plt_slot(const plt_slot&) = delete;
  plt_slot& operator=(const plt_slot&) = delete;

  pointer target = nullptr;

  [[nodiscard]] explicit operator bool() const { return target != nullptr; }

  template <typename... Args>
  decltype(auto) operator()(Args&&... args) const {
    return target(std::forward<Args>(args)...);
  }

private:
  // Descriptor addresses were cast from this entry's own type, so casting
  // back restores the original function pointer.
  static void assign(void* slot, detail::plt_function value) noexcept {
    static_cast<plt_slot*>(slot)->target = reinterpret_cast<pointer>(value);
  }

  detail::plt_slot_record record_;
};

} // namespace neko::wasm
