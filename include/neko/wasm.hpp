#pragma once

#include <neko/backend/session_driver.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace neko::wasm::detail {

using plt_function = void (*)();

struct plt_slot_record {
  std::string_view group;
  std::string_view entry;
  plt_function* target;
  plt_slot_record* next = nullptr;
};

void register_plt_slot(plt_slot_record& record) noexcept;

} // namespace neko::wasm::detail

namespace neko::wasm {
#if defined(__EMSCRIPTEN__)
/// Discover build-generated groups, initially disabled, and own their browser
/// transport, scheduling and preparation. Pass the result to reload_session.
/// Only one browser backend may exist per page because PLT slots are global.
[[nodiscard]] std::unique_ptr<backend::session_driver> create_backend();
#endif

/// A static-duration function pointer rewritten when a generation activates.
/// Define one slot and forwarding function per entry in the application header
/// passed as PLT_HEADER to nekomata_add_reload_group. Call only after the
/// group's first generation has activated.
template <typename Signature>
class plt_slot {
public:
  plt_slot(std::string_view group, std::string_view entry)
      : record_{group, entry, reinterpret_cast<detail::plt_function*>(&target)} {
    detail::register_plt_slot(record_);
  }

  plt_slot(const plt_slot&) = delete;
  plt_slot& operator=(const plt_slot&) = delete;

  Signature target = nullptr;

  [[nodiscard]] explicit operator bool() const { return target != nullptr; }

  template <typename... Args>
  decltype(auto) operator()(Args&&... args) const {
    return target(std::forward<Args>(args)...);
  }

private:
  detail::plt_slot_record record_;
};

} // namespace neko::wasm
