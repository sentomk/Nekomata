#pragma once

#include "flock.hpp"

#include <neko/wasm.hpp>

// One slot per reloadable entry, typed from its declaration in flock.hpp.
// The backend rewrites these at the page's safe point when a generation
// activates. plt.cpp defines flock::step and flock::describe in the main
// module as forwarders through the slots, so the page calls them directly.
namespace flock::plt {

inline neko::wasm::plt_slot<decltype(flock::step)> step{"flock", "step"};
inline neko::wasm::plt_slot<decltype(flock::describe)> describe{"flock", "describe"};

} // namespace flock::plt
