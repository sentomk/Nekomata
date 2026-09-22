#pragma once

#include <neko/backend/session_driver.hpp>

#include <memory>
#include <string_view>
#include <type_traits>

namespace neko::wasm {
class prepared_module;

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
  friend entry_set acquire(const ::neko::reload_session&, std::string_view);
  using function = void (*)();
  [[nodiscard]] function lookup(std::string_view name) const;
  std::shared_ptr<const prepared_module> module_;
};

#if defined(__EMSCRIPTEN__)
/// Discover build-generated groups, initially disabled, and own their browser
/// transport, scheduling and preparation. Pass the result to reload_session.
[[nodiscard]] std::unique_ptr<backend::session_driver> create_backend();

/// Acquire an owning entry snapshot from an already registered group. Empty
/// before its first activation; unknown groups are configuration errors. This
/// never registers a group. Calls use the session's single application event loop.
[[nodiscard]] entry_set acquire(const ::neko::reload_session& session, std::string_view group_id);
#endif
} // namespace neko::wasm
