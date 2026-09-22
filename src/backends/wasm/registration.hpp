#pragma once

#include "session.hpp"
#include <neko/detail/wasm_registration.hpp>

#include <string_view>

namespace neko::wasm::detail {
[[nodiscard]] std::vector<group_registration> read_group_records(const group_record* first);
[[nodiscard]] std::vector<group_registration> discover_groups();

/// Rewrite every PLT slot registered for `group` with the activated
/// module's entry addresses. Runs inside the caller's safe point, next to
/// activation itself; unknown slot names leave the old target untouched.
void apply_plt_slots(std::string_view group, const prepared_module& module) noexcept;
} // namespace neko::wasm::detail
