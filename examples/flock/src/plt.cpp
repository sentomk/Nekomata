#include "plt.hpp"

// The main module's definitions of the reloadable entries. Each side module
// defines its own hidden copies; these forward to whichever generation is
// active. Before the first generation arrives the slots are empty.
namespace flock {

void step(world_state* world) {
  if (plt::step) {
    plt::step(world);
  }
}

const char* describe() {
  return plt::describe ? plt::describe() : "waiting for the first generation";
}

} // namespace flock
