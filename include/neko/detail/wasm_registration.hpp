#pragma once

// Internal build/runtime contract. Only generated registration TUs use this
// header; application code uses the public factory, never these records.
#include <span>
#include <string_view>

namespace neko::wasm::detail {
struct group_record {
  std::string_view group_id;
  std::string_view manifest_url;
  std::string_view abi_id;
  std::span<const std::string_view> entries;
  group_record* next = nullptr;
};

// Called during main-module startup with static-lifetime data; never allocates
// or throws. Factories validate and copy the completed registry after startup.
void register_group(group_record& record) noexcept;
} // namespace neko::wasm::detail
