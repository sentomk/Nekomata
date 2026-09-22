#pragma once

#include "candidate.hpp"
#include <neko/wasm.hpp>

namespace neko::wasm::detail {
struct group_state {
  std::string id;
  std::string url;
  module_contract contract;
  std::shared_ptr<const prepared_module> active;
  bool attached = false;
};

// A session's exclusive claim; construction failure releases earlier claims.
class group_binding {
public:
  explicit group_binding(const group& value);
  ~group_binding();
  group_binding(const group_binding&) = delete;
  group_binding& operator=(const group_binding&) = delete;
  [[nodiscard]] const group_state& config() const noexcept { return *state_; }
  void publish(std::shared_ptr<const prepared_module> active) noexcept;

private:
  std::shared_ptr<group_state> state_;
};
} // namespace neko::wasm::detail
