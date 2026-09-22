#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace neko::wasm {
class prepared_module;
namespace detail {
struct group_state;
class group_binding;
} // namespace detail

/// One immutable generation. Keep this value alive throughout its entry calls.
/// Signatures and world layout must match the group's cooperative ABI contract.
class entry_set {
public:
  entry_set() = default;
  [[nodiscard]] explicit operator bool() const noexcept { return module_ != nullptr; }
  template <class signature>
  [[nodiscard]] signature* get(std::string_view name) const {
    static_assert(std::is_function_v<signature>, "get expects a function signature");
    return reinterpret_cast<signature*>(lookup(name));
  }

private:
  friend class group;
  using function = void (*)();
  [[nodiscard]] function lookup(std::string_view name) const;
  std::shared_ptr<const prepared_module> module_;
};

/// Explicit browser registration. Copies share the active generation; one live
/// session may own a registration. All access is serialized on its event loop.
class group {
public:
  group(std::string group_id, std::string manifest_url, std::string abi_id,
        std::vector<std::string> entries);
  /// Empty before the first successful update. Previously acquired values do
  /// not change on update, unwatch, or session destruction.
  [[nodiscard]] entry_set acquire() const noexcept;

private:
  friend class detail::group_binding;
  std::shared_ptr<detail::group_state> state_;
};
} // namespace neko::wasm
