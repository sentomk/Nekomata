#pragma once

#include "../world.hpp"

#include <neko/wasm.hpp>

namespace public_plt {

namespace alpha {
inline neko::wasm::plt_slot<update_fn> update{"alpha", "update_world"};
inline neko::wasm::plt_slot<identify_fn> identity{"alpha", "identify"};

inline void update_world(world_state* world) {
  update(world);
}

inline std::uint32_t identify() {
  return identity ? identity() : 0;
}
} // namespace alpha

namespace beta {
inline neko::wasm::plt_slot<update_fn> update{"beta", "update_world"};
inline neko::wasm::plt_slot<identify_fn> identity{"beta", "identify"};

inline void update_world(world_state* world) {
  update(world);
}

inline std::uint32_t identify() {
  return identity ? identity() : 0;
}
} // namespace beta

} // namespace public_plt
