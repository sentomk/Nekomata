#include "candidate.hpp"
#include <neko/wasm.hpp>

#include <stdexcept>
#include <string>

namespace neko::wasm {
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
} // namespace neko::wasm
