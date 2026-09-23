#pragma once

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

/// A static-duration function pointer that the browser backend rewrites at
/// activation. The application defines one slot and forwarding function per
/// entry in the header passed as PLT_HEADER to nekomata_add_reload_group.
/// Call only after the first generation has activated for that group.
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
