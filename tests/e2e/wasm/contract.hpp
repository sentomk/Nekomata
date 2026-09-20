#pragma once

#include <backends/wasm/candidate.hpp>
#include <cstdint>

// Application state and signatures are test-specific, independent of the loader.
struct world_state {
  std::uint32_t tick_count;
  float position;
  float velocity;
  std::uint32_t last_generation;

  bool operator==(const world_state&) const = default;
};

using identify_fn = std::uint32_t (*)();
using update_fn = void (*)(world_state*);

inline neko::wasm::module_contract flock_contract() {
  return {"flock-test-v1", {"identify", "update_world"}};
}
