#pragma once

#include "session.hpp"
#include <neko/detail/wasm_registration.hpp>

namespace neko::wasm::detail {
[[nodiscard]] std::vector<group_registration> read_group_records(const group_record* first);
[[nodiscard]] std::vector<group_registration> discover_groups();
} // namespace neko::wasm::detail
