#pragma once

#include "world.hpp"

namespace wasm_fixture {

std::uint32_t identify();
void update_world(world_state* world);

} // namespace wasm_fixture
