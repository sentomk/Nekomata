#pragma once

#include "../generation.hpp"
#include "../world.hpp"

#include <neko/wasm.hpp>

namespace public_plt {

namespace alpha {
inline neko::wasm::plt_slot<decltype(wasm_fixture::update_world)> update{"alpha", "update_world"};
inline neko::wasm::plt_slot<decltype(wasm_fixture::identify)> identity{"alpha", "identify"};

inline void update_world(world_state* world) {
  update(world);
}

inline std::uint32_t identify() {
  return identity ? identity() : 0;
}
} // namespace alpha

namespace beta {
inline neko::wasm::plt_slot<decltype(wasm_fixture::update_world)> update{"beta", "update_world"};
inline neko::wasm::plt_slot<decltype(wasm_fixture::identify)> identity{"beta", "identify"};

inline void update_world(world_state* world) {
  update(world);
}

inline std::uint32_t identify() {
  return identity ? identity() : 0;
}
} // namespace beta

} // namespace public_plt
