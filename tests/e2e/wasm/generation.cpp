#include "world.hpp"

#include <neko/detail/wasm_module_descriptor.hpp>

namespace {

std::uint32_t identify() {
  return GENERATION_ID;
}

[[maybe_unused]] void update_world(world_state* world) {
#if GENERATION_ID == 2
  // Only generation B contains this branch; this is a code change, not an input.
  if (world->position > 10.0f) {
    world->velocity = -1.0f;
  }
#endif
  world->position += world->velocity;
  ++world->tick_count;
  world->last_generation = GENERATION_ID;
}

const neko::wasm::module_entry entries[] = {
    {"identify", reinterpret_cast<neko::wasm::module_function>(identify)},
    {"update_world",
     MISSING_UPDATE ? nullptr : reinterpret_cast<neko::wasm::module_function>(update_world)},
};

const neko::wasm::module_descriptor descriptor{
    {INTERFACE_VERSION, sizeof(neko::wasm::module_descriptor)}, "flock-test-v1", 2, entries};

} // namespace

extern "C" const neko::wasm::module_header* neko_wasm_descriptor() {
  return &descriptor.header;
}
