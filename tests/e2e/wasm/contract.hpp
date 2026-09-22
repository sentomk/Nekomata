#pragma once

#include "world.hpp"

#include <backends/wasm/candidate.hpp>

inline neko::wasm::module_contract flock_contract(std::string_view sha256) {
  return {"flock-test-v1", {"identify", "update_world"}, std::string{sha256}};
}
