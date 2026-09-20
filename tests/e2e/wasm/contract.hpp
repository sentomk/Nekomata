#pragma once

#include <cstdint>

// A test-only interface; this is not the future neko WASM ABI.
struct world_state {
  std::uint32_t tick_count;
  float position;
  float velocity;
  std::uint32_t last_generation;

  bool operator==(const world_state&) const = default;
};

struct generation_descriptor {
  std::uint32_t interface_version;
  std::uint32_t generation_id;
  std::uint32_t (*identify)();
  void (*update_world)(world_state*);
};

using descriptor_fn = const generation_descriptor* (*)();
