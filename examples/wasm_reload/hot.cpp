#include "contract.hpp"

#include <neko/detail/wasm_module_descriptor.hpp>

// One reloadable behavior generation. BEHAVIOR selects the physics; every
// variant shares the descriptor contract, so a publish swaps behavior on
// the existing world without any state migration.

namespace {

std::uint32_t identify() {
  return BEHAVIOR;
}

void bounce_axis(float& position, float& velocity, float low, float high) {
  if (position < low) {
    position = low + (low - position);
    velocity = -velocity;
  } else if (position > high) {
    position = high - (position - high);
    velocity = -velocity;
  }
}

void update_world(demo::world_state* world) {
#if BEHAVIOR == 1
  // Linear drift: straight diagonal paths with elastic wall bounces.
  world->x += world->vx;
  world->y += world->vy;
  bounce_axis(world->x, world->vx, 8.0f, demo::world_width - 8.0f);
  bounce_axis(world->y, world->vy, 8.0f, demo::world_height - 8.0f);
#elif BEHAVIOR == 2
  // Gravity with damped floor bounces: visible parabolic arcs.
  world->vy += 0.45f;
  world->x += world->vx;
  world->y += world->vy;
  bounce_axis(world->x, world->vx, 8.0f, demo::world_width - 8.0f);
  if (world->y > demo::world_height - 8.0f) {
    world->y = demo::world_height - 8.0f;
    world->vy = -world->vy * 0.92f;
    world->vx *= 0.995f;
  }
  if (world->y < 8.0f) {
    world->y = 8.0f;
    world->vy = -world->vy;
  }
#else
  // Spring pull toward the canvas center with elastic walls: orbiting sway.
  constexpr float cx = demo::world_width / 2.0f;
  constexpr float cy = demo::world_height / 2.0f;
  world->vx += (cx - world->x) * 0.004f;
  world->vy += (cy - world->y) * 0.004f;
  world->x += world->vx;
  world->y += world->vy;
  bounce_axis(world->x, world->vx, 8.0f, demo::world_width - 8.0f);
  bounce_axis(world->y, world->vy, 8.0f, demo::world_height - 8.0f);
#endif
  ++world->tick_count;
  world->behavior = BEHAVIOR;
}

const neko::wasm::module_entry entries[] = {
    {"identify", reinterpret_cast<neko::wasm::module_function>(identify)},
    {"update_world", reinterpret_cast<neko::wasm::module_function>(update_world)},
};

const neko::wasm::module_descriptor descriptor{
    {neko::wasm::module_interface_version, sizeof(neko::wasm::module_descriptor)},
    demo::abi_id,
    2,
    entries};

} // namespace

extern "C" const neko::wasm::module_header* neko_wasm_descriptor() {
  return &descriptor.header;
}
