#include "contract.hpp"
#include "fixture_digests.h"

#include <backends/wasm/emscripten_loader.hpp>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdlib>
#include <memory>

namespace {
using namespace neko::wasm;

world_state world{0, 100.0f, 1.0f, 0};
world_state* const original_world = &world;
emscripten_loader loader;
active_module active;
std::unique_ptr<candidate> pending;
std::shared_ptr<const prepared_module> generation_a;
std::shared_ptr<const prepared_module> generation_b;
std::uint32_t ticks_during_load = 0;
std::uint32_t ticks_while_ready = 0;
std::uint32_t ticks_after_commit = 0;
std::uint32_t rejection_index = 0;
std::uint32_t rejection_tick = 0;
bool rejection_reported = false;

struct rejection_case {
  const char* path;
  const char* digest;
  candidate_error expected;
};
// The tampered case names real bytes with the wrong digest: the mismatch
// must be caught before instantiation, without touching the active module.
constexpr const char* tampered_digest =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr rejection_case rejection_cases[] = {
    {"incompatible.wasm", incompatible_digest, candidate_error::incompatible},
    {"incomplete.wasm", incomplete_digest, candidate_error::invalid_descriptor},
    {"absent.wasm", a_digest, candidate_error::load_failed},
    {"b.wasm", tampered_digest, candidate_error::integrity},
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

std::uint32_t identify(const prepared_module& module) {
  return reinterpret_cast<identify_fn>(module.entry("identify"))();
}

void prepare(const char* path, std::string_view digest) {
  pending = std::make_unique<candidate>(loader, path, flock_contract(digest));
}

bool frame(double, void*) {
  if (!generation_a) {
    require(pending->status() != candidate_status::rejected, "A rejected");
    if (pending->status() != candidate_status::ready) {
      require(!active.activate(*pending), "activated unfinished A");
      return true;
    }
    require(active.activate(*pending), "A activation failed");
    generation_a = active.current();
    require(identify(*generation_a) == 1, "A identity");
    prepare("b.wasm", b_digest);
    return true;
  }

  if (!generation_b) {
    require(pending->status() != candidate_status::rejected, "B rejected");
    require(active.current() == generation_a, "preparation changed active module");
    require(identify(*generation_a) == 1, "loading B changed A's entry");
    if (pending->status() == candidate_status::ready && ticks_while_ready == 3) {
      const auto before = world;
      require(active.activate(*pending), "B activation failed");
      require(pending->status() == candidate_status::activated, "candidate not consumed");
      require(!active.activate(*pending), "candidate activated twice");
      generation_b = active.current();
      require(generation_b != generation_a && identify(*generation_b) == 2, "B identity");
      require(generation_b->entry("update_world") != generation_a->entry("update_world"),
              "same update function");
      require(&world == original_world && world == before, "commit changed world state");
      require(world.last_generation == 1, "candidate executed during commit");
      pending.reset();
    }
  } else if (pending && !rejection_reported) {
    require(pending->status() != candidate_status::ready, "invalid candidate became ready");
    if (pending->status() == candidate_status::rejected) {
      const auto before = world;
      require(pending->error() == rejection_cases[rejection_index].expected,
              "wrong rejection reason");
      require(!pending->message().empty(), "missing rejection diagnostic");
      require(!active.activate(*pending), "rejected candidate activated");
      require(active.current() == generation_b && world == before,
              "rejection changed active state");
      rejection_reported = true;
      rejection_tick = world.tick_count;
    }
  }

  const auto before = world;
  const auto frame_generation = active.current();
  reinterpret_cast<update_fn>(frame_generation->entry("update_world"))(&world);
  require(&world == original_world, "world address changed");
  require(world.tick_count == before.tick_count + 1, "tick count reset or skipped");
  require(world.last_generation == identify(*frame_generation), "mixed generation entries");
  if (frame_generation == generation_a) {
    require(world.velocity == before.velocity &&
                world.position == before.position + before.velocity,
            "A behavior changed before activation");
    if (pending->status() == candidate_status::loading) {
      ++ticks_during_load;
      if (ticks_during_load == 1) {
        release_download();
      }
    } else {
      require(ticks_during_load > 0, "A did not run during B download");
      ++ticks_while_ready;
    }
  } else {
    require(world.velocity == -1.0f && world.position == before.position - 1.0f,
            "B did not execute its new branch on the existing world");
    ++ticks_after_commit;
    if (ticks_after_commit >= 3 && !pending) {
      require(ticks_while_ready == 3, "candidate did not wait for the safe point");
      prepare(rejection_cases[rejection_index].path, rejection_cases[rejection_index].digest);
    }
    if (rejection_reported && world.tick_count >= rejection_tick + 3) {
      pending.reset();
      rejection_reported = false;
      ++rejection_index;
      if (rejection_index == 4) {
        require(identify(*generation_a) == 1, "old code no longer callable");
        report(1, "backend candidates preserved state across activation and four rejection cases");
        return false;
      }
    }
  }
  return true;
}

} // namespace

int main() {
  prepare("a.wasm", a_digest);
  emscripten_request_animation_frame_loop(frame, nullptr);
  emscripten_exit_with_live_runtime();
}
