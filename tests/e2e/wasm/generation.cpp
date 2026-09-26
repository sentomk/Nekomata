#include "generation.hpp"

#if !defined(NEKO_GENERATED_DESCRIPTOR)
#include <neko/detail/wasm_module_descriptor.hpp>
#endif

namespace wasm_fixture {
namespace {
// Taking a local's address gives update_world a stack frame. Leaf functions
// without one never run the side module's stack-pointer checks, which a
// release page must be able to serve.
void advance(float& position, float velocity) {
  position += velocity;
}
} // namespace

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
  float position = world->position;
  advance(position, world->velocity);
  world->position = position;
  ++world->tick_count;
  world->last_generation = GENERATION_ID;
}

} // namespace wasm_fixture

#if !defined(NEKO_GENERATED_DESCRIPTOR)
namespace {
const neko::wasm::module_entry entries[] = {
    {"identify", reinterpret_cast<neko::wasm::module_function>(wasm_fixture::identify)},
    {"update_world",
     MISSING_UPDATE ? nullptr
                    : reinterpret_cast<neko::wasm::module_function>(wasm_fixture::update_world)},
};

const neko::wasm::module_descriptor descriptor{
    {INTERFACE_VERSION, sizeof(neko::wasm::module_descriptor)}, "flock-test-v1", 2, entries};
} // namespace

extern "C" __attribute__((visibility("default"))) const neko::wasm::module_header*
neko_wasm_descriptor() {
  return &descriptor.header;
}
#endif
