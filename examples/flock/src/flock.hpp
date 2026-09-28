#pragma once

#include "world.hpp"

// The reloadable entries. The build generates each generation's descriptor
// from these declarations, and the page's PLT slots take their types from
// them. Changing a signature changes the ABI identity, like a layout change.
namespace flock {

// Advance every boid by one frame.
void step(world_state* world);

// A short label for the HUD, so each generation can name itself.
const char* describe();

} // namespace flock
