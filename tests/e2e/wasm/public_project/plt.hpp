#pragma once

#include "../world.hpp"

#include <neko/wasm/plt.hpp>

namespace public_plt {

inline neko::wasm::plt_slot<update_fn> alpha_update{"alpha", "update_world"};

inline void update_world(world_state* world) {
  alpha_update(world);
}

} // namespace public_plt
