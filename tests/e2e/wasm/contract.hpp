#pragma once

#include <cstdint>

// A test-only interface; this is not the future neko WASM ABI.
struct generation_descriptor {
  std::uint32_t interface_version;
  std::uint32_t generation_id;
  std::uint32_t (*identify)();
};

using descriptor_fn = const generation_descriptor* (*)();
