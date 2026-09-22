#pragma once

#include <cstdint>

// The fixture ABI belongs to the application, independently of the backend.
struct world_state {
  std::uint32_t tick_count;
  float position;
  float velocity;
  std::uint32_t last_generation;

  bool operator==(const world_state&) const = default;
};

using identify_fn = std::uint32_t (*)();
using update_fn = void (*)(world_state*);
