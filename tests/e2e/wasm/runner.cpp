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
std::uint32_t rejection_index = 0;
std::uint32_t rejection_tick = 0;
bool rejection_pending = false;
bool rejection_reported = false;

enum class rejection_code { none, incompatible, missing_entry, load_failed };

struct rejection_case {
  const char* path;
  rejection_code expected;
};

constexpr rejection_case rejection_cases[] = {
    {"incompatible.wasm", rejection_code::incompatible},
    {"incomplete.wasm", rejection_code::missing_entry},
    {"absent.wasm", rejection_code::load_failed},
};

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });

EM_JS(void, release_download, (), { fetch("/a-frame", {method : "POST"}); });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

const generation_descriptor* descriptor_from(void* handle, rejection_code& code) {
  code = rejection_code::none;
  auto entry = reinterpret_cast<descriptor_fn>(dlsym(handle, "get_generation_descriptor"));
  if (entry == nullptr) {
    code = rejection_code::missing_entry;
    return nullptr;
  }
  const auto* descriptor = entry();
  if (descriptor == nullptr) {
    code = rejection_code::missing_entry;
    return nullptr;
  }
  // Inspect the version prefix before accessing the version-specific entries.
  if (descriptor->interface_version != 1) {
    code = rejection_code::incompatible;
    return nullptr;
  }
  if (descriptor->identify == nullptr || descriptor->update_world == nullptr) {
    code = rejection_code::missing_entry;
    return nullptr;
  }
  return descriptor;
}

const generation_descriptor* require_descriptor(void* handle) {
  rejection_code code;
  const auto* descriptor = descriptor_from(handle, code);
  require(code == rejection_code::none && descriptor != nullptr, "valid descriptor rejected");
  return descriptor;
}

void load_failed(void*) {
  report(0, dlerror());
}

void record_rejection(rejection_code code) {
  require(rejection_pending && !rejection_reported, "unexpected rejection callback");
  require(code == rejection_cases[rejection_index].expected, "wrong rejection reason");
  require(active->generation_id == 2 && candidate == nullptr,
          "rejection changed active generation");
  rejection_tick = world.tick_count;
  rejection_reported = true;
}

void rejected_load(void*, void*) {
  require(false, "missing artifact unexpectedly loaded");
}

void expected_load_failure(void*) {
  require(dlerror() != nullptr, "failed load did not provide a diagnostic");
  record_rejection(rejection_code::load_failed);
}

void loaded_invalid(void*, void* handle) {
  const auto before = world;
  const auto* previous_active = active;
  rejection_code code;
  const auto* proposed = descriptor_from(handle, code);
  require(proposed == nullptr, "invalid descriptor accepted");
  require(world == before && active == previous_active, "rejection mutated live state");
  record_rejection(code);
}

void start_rejection_case() {
  rejection_pending = true;
  rejection_reported = false;
  const auto& test = rejection_cases[rejection_index];
  if (test.expected == rejection_code::load_failed) {
    emscripten_dlopen(test.path, RTLD_NOW | RTLD_LOCAL, nullptr, rejected_load,
                      expected_load_failure);
  } else {
    emscripten_dlopen(test.path, RTLD_NOW | RTLD_LOCAL, nullptr, loaded_invalid, load_failed);
  }
}

void loaded_b(void*, void* handle) {
  const auto before = world;
  candidate = require_descriptor(handle);
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
      if (ticks_during_load == 1) {
        release_download();
      }
    } else {
      ++ticks_while_ready;
    }
  } else {
    require(world.velocity == -1.0f && world.position == before.position - 1.0f,
            "B did not execute its new branch on the existing world");
    ++ticks_after_commit;
    if (ticks_after_commit >= 3 && !rejection_pending) {
      require(ticks_while_ready == 3, "ready candidate did not wait for the safe point");
      require(generation_a->identify() == 1, "old code no longer callable");
      start_rejection_case();
    }
    if (rejection_reported && world.tick_count >= rejection_tick + 3) {
      ++rejection_index;
      rejection_pending = false;
      rejection_reported = false;
      if (rejection_index == 3) {
        report(1, "state-preserving A to B commit; incompatible, incomplete and absent candidates "
                  "rejected; B continued for three frames after each rejection");
        return false;
      }
    }
  }
  return true;
}

void loaded_a(void*, void* handle) {
  generation_a = require_descriptor(handle);
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
