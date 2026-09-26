#pragma once

#include "session.hpp"
#include <neko/detail/wasm_registration.hpp>
#include <neko/wasm.hpp>

#include <string_view>
#include <vector>

namespace neko::wasm::detail {
[[nodiscard]] std::vector<group_registration> read_group_records(const group_record* first);
[[nodiscard]] std::vector<group_registration> discover_groups();

/// Reject a PLT slot whose group is not registered or whose entry is not in
/// that group's contract; such a slot would otherwise stay empty forever.
/// The second form checks every slot registered by the page so far.
void validate_plt_slots(const std::vector<group_registration>& groups,
                        const plt_slot_record* first);
void validate_plt_slots(const std::vector<group_registration>& groups);

/// Rewrite every PLT slot registered for `group` with the activated
/// module's entry addresses. Runs inside the caller's safe point, next to
/// activation itself. Slots were validated before observation started;
/// an entry the module lacks still leaves the old target untouched.
void apply_plt_slots(std::string_view group, const prepared_module& module) noexcept;
} // namespace neko::wasm::detail
