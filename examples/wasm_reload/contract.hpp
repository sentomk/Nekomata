#pragma once

#include <backends/wasm/candidate.hpp>
#include <cstdint>

// Shared between the persistent main module and every reloadable side
// module. The `abi_id` pins this layout plus both entry signatures: a
// behavior change ships under the same identity, an ABI change does not.
namespace demo {

inline constexpr char abi_id[] = "demo-ball-v1";
inline constexpr char group_id[] = "demo-ball";

inline constexpr float world_width = 800.0f;
inline constexpr float world_height = 480.0f;

// All persistent state belongs to the main module. Side-module code only
// receives a pointer to this structure.
struct world_state {
  std::uint32_t tick_count;
  float x;
  float y;
  float vx;
  float vy;
  std::uint32_t behavior;
};

using identify_fn = std::uint32_t (*)();
using update_fn = void (*)(world_state*);

inline neko::wasm::module_contract contract(std::string_view sha256) {
  return {abi_id, {"identify", "update_world"}, std::string{sha256}};
}

} // namespace demo
