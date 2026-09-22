#include "group.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace neko::wasm {
namespace {
bool valid_text(const std::string& value) {
  return !value.empty() && value.find('\0') == std::string::npos;
}
} // namespace

group::group(std::string group_id, std::string manifest_url, std::string abi_id,
             std::vector<std::string> entries) {
  if (!valid_text(group_id) || !valid_text(manifest_url) || !valid_text(abi_id) ||
      entries.empty()) {
    throw std::invalid_argument("wasm group: nonempty ID, URL, ABI and entries are required");
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!valid_text(entries[i]) ||
        std::find(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(i), entries[i]) !=
            entries.begin() + static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument("wasm group: entry names must be nonempty and unique");
    }
  }
  state_ = std::make_shared<detail::group_state>(
      detail::group_state{std::move(group_id),
                          std::move(manifest_url),
                          {std::move(abi_id), std::move(entries), {}},
                          {},
                          false});
}

entry_set group::acquire() const noexcept {
  entry_set out;
  if (state_) {
    out.module_ = state_->active;
  }
  return out;
}

entry_set::function entry_set::lookup(std::string_view name) const {
  if (!module_) {
    throw std::runtime_error("wasm entry_set: no active generation");
  }
  const auto address = module_->entry(name);
  if (address == nullptr) {
    throw std::invalid_argument("wasm entry_set: unknown entry '" + std::string{name} + "'");
  }
  return address;
}

detail::group_binding::group_binding(const group& value) : state_(value.state_) {
  if (!state_) {
    throw std::invalid_argument("wasm group: moved-from registration");
  }
  if (state_->attached) {
    throw std::invalid_argument("wasm group: registration already belongs to a session");
  }
  state_->attached = true;
}

detail::group_binding::~group_binding() {
  state_->attached = false;
}

void detail::group_binding::publish(std::shared_ptr<const prepared_module> active) noexcept {
  state_->active = std::move(active);
}
} // namespace neko::wasm
