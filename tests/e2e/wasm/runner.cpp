#include "contract.hpp"

#include <dlfcn.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdlib>

namespace {

world_state world{0, 100.0f, 1.0f, 0};
world_state* const original_world = &world;
const generation_descriptor* generation_a = nullptr;
const generation_descriptor* active = nullptr;
const generation_descriptor* candidate = nullptr;
std::uint32_t ticks_during_load = 0;
std::uint32_t ticks_while_ready = 0;
std::uint32_t ticks_after_commit = 0;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

const generation_descriptor* descriptor_from(void* handle) {
  auto entry = reinterpret_cast<descriptor_fn>(dlsym(handle, "get_generation_descriptor"));
  require(entry != nullptr, "descriptor export missing");
  const auto* descriptor = entry();
  require(descriptor != nullptr, "null descriptor");
  require(descriptor->interface_version == 1, "unexpected interface version");
  require(descriptor->identify != nullptr && descriptor->update_world != nullptr,
          "incomplete descriptor");
  return descriptor;
}

void load_failed(void*) {
  report(0, dlerror());
}

void loaded_b(void*, void* handle) {
  const auto before = world;
  candidate = descriptor_from(handle);
  require(candidate != generation_a, "handles resolved the same descriptor");
  require(candidate->generation_id == 2 && candidate->identify() == 2, "B identity");
  require(generation_a->identify() == 1, "A changed after loading B");
  require(candidate->update_world != generation_a->update_world, "same update function");
  require(active == generation_a && world == before, "preparation changed active state");
  require(ticks_during_load > 0, "A did not run during B download");
}

bool frame(double, void*) {
  if (candidate != nullptr && ticks_while_ready == 3) {
    const auto before = world;
    require(active == generation_a, "candidate became active before commit");
    active = candidate;
    candidate = nullptr;
    require(&world == original_world && world == before, "commit changed world state");
    require(world.last_generation == 1, "candidate executed during commit");
  }

  const auto before = world;
  const auto* frame_generation = active;
  frame_generation->update_world(&world);
  require(&world == original_world, "world address changed");
  require(world.tick_count == before.tick_count + 1, "tick count reset or skipped");
  require(world.last_generation == frame_generation->identify(), "mixed generation entries");
  if (frame_generation == generation_a) {
    require(world.velocity == before.velocity &&
                world.position == before.position + before.velocity,
            "A behavior changed before activation");
    if (candidate == nullptr) {
      ++ticks_during_load;
    } else {
      ++ticks_while_ready;
    }
  } else {
    require(world.velocity == -1.0f && world.position == before.position - 1.0f,
            "B did not execute its new branch on the existing world");
    ++ticks_after_commit;
    if (ticks_after_commit == 3) {
      require(ticks_while_ready == 3, "ready candidate did not wait for the safe point");
      require(generation_a->identify() == 1, "old code no longer callable");
      report(1, "A ran during download and readiness; frame commit preserved state; B bounced");
      return false;
    }
  }
  return true;
}

void loaded_a(void*, void* handle) {
  generation_a = descriptor_from(handle);
  require(generation_a->generation_id == 1, "A descriptor identity");
  require(generation_a->identify() == 1, "A function identity");
  active = generation_a;
  emscripten_request_animation_frame_loop(frame, nullptr);
  emscripten_dlopen("b.wasm", RTLD_NOW | RTLD_LOCAL, nullptr, loaded_b, load_failed);
}

} // namespace

int main() {
  emscripten_dlopen("a.wasm", RTLD_NOW | RTLD_LOCAL, nullptr, loaded_a, load_failed);
  emscripten_exit_with_live_runtime();
}
