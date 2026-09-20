#include "contract.hpp"

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

const generation_descriptor descriptor{INTERFACE_VERSION, GENERATION_ID, identify,
                                       MISSING_UPDATE ? nullptr : update_world};

} // namespace

extern "C" const generation_descriptor* get_generation_descriptor() {
  return &descriptor;
}
