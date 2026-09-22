#pragma once

// The demo's PLT: one typed slot and one forwarding trampoline per entry.
// The build adapter compiles this into the main module (PLT_HEADER), so the
// page's call sites stay ordinary direct calls — demo::update_world(world) —
// and the reload backend rewrites the slots at its safe point. This is the
// whole application-side ceremony; entry signatures are application
// knowledge and stay in plain, readable code.

#include "contract.hpp"

#include <backends/wasm/plt.hpp>

namespace demo::plt {

inline neko::wasm::plt_slot<demo::update_fn> update_world{demo::group_id, "update_world"};
inline neko::wasm::plt_slot<demo::identify_fn> identify{demo::group_id, "identify"};

} // namespace demo::plt

namespace demo {

inline void update_world(world_state* world) {
  plt::update_world(world);
}

inline std::uint32_t identify() {
  return plt::identify();
}

} // namespace demo
