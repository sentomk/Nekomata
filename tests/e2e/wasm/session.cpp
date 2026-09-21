#include "contract.hpp"

#include <backends/wasm/emscripten_loader.hpp>
#include <backends/wasm/emscripten_scheduler.hpp>
#include <backends/wasm/session.hpp>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdlib>
#include <memory>
#include <utility>

namespace {
using namespace neko::wasm;

// Observe real HTTP and loader completions without replacing their behavior.
class observed_fetcher final : public manifest_fetcher {
public:
  unsigned started = 0;
  unsigned completed = 0;

  void fetch(std::string url, completion complete) override {
    ++started;
    inner_.fetch(std::move(url), [this, complete = std::move(complete)](manifest_text result) {
      complete(std::move(result));
      ++completed;
    });
  }

private:
  emscripten_manifest_fetcher inner_;
};

class observed_loader final : public module_loader {
public:
  unsigned completed = 0;

  void open(std::string path, std::string_view sha256, completion complete) override {
    inner_.open(std::move(path), sha256,
                [this, complete = std::move(complete)](module_load_result result) {
                  complete(std::move(result));
                  ++completed;
                });
  }

private:
  emscripten_loader inner_;
};

enum class step {
  disabled,
  loading_a,
  running_a,
  published_b,
  loading_b,
  paused_b,
  running_b,
  rejecting_c,
  rejected_c
};
step progress = step::disabled;
world_state world{0, 100.0f, 1.0f, 0};
world_state* const original_world = &world;
observed_loader loader;
observed_fetcher fetcher;
emscripten_poll_scheduler scheduler;
std::unique_ptr<reload_session> session;
std::unique_ptr<poll_subscription> pulse;
std::shared_ptr<const prepared_module> generation_a;
std::shared_ptr<const prepared_module> generation_b;
std::string applied_a;
std::string applied_b;
unsigned pulses = 0;
unsigned paused_pulse = 0;
unsigned paused_fetches = 0;
unsigned accepted = 0;
unsigned ignored = 0;
unsigned rejected_ignored = 0;
unsigned ready_frames = 0;
unsigned b_frames = 0;
unsigned rejected_tick = 0;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });
EM_JS(void, control, (const char* path), {
  window.control_done = false;
  fetch(UTF8ToString(path), {method : "POST"}).then(function(response) {
    if (!response.ok)
      throw new Error("fixture control failed: " + response.status);
    window.control_done = true;
  });
});
EM_JS(bool, control_done, (), { return !!window.control_done; });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

std::uint32_t identify(const prepared_module& module) {
  return reinterpret_cast<identify_fn>(module.entry("identify"))();
}

update_result safe_update() {
  const auto before = world;
  auto result = session->update();
  require(&world == original_world && world == before, "session update changed persistent state");
  return result;
}

void require_applied(const update_result& result) {
  require(result.events.size() == 1, "expected exactly one generation transaction");
  const auto& event = result.events.front();
  require(event.status == update_status::applied && event.code == candidate_error::none &&
              event.redirected_entry_count == 2 && !event.generation_id.empty(),
          "generation did not apply its entire entry set");
}

void pause() {
  session->unwatch("flock");
  session->unwatch();
  paused_pulse = pulses;
  paused_fetches = fetcher.started;
}

void require_paused() {
  require(safe_update().events.empty(), "paused session consumed a transaction");
  require(fetcher.started == paused_fetches, "paused session started another manifest fetch");
  require(session->current() == generation_a, "paused session replaced the active generation");
}

void advance_world() {
  const auto current = session->current();
  const auto before = world;
  reinterpret_cast<update_fn>(current->entry("update_world"))(&world);
  require(&world == original_world && world.tick_count == before.tick_count + 1,
          "world identity or age reset");
  require(world.last_generation == identify(*current), "frame mixed generation entries");
  if (current == generation_a) {
    require(world.velocity == before.velocity &&
                world.position == before.position + before.velocity,
            "A behavior changed before the commit");
  } else {
    require(current == generation_b && world.velocity == -1.0f &&
                world.position == before.position - 1.0f,
            "B did not run its new code branch on the existing world");
    ++b_frames;
  }
}

bool frame(double, void*) {
  switch (progress) {
  case step::disabled:
    require(safe_update().events.empty() && !session->current() && fetcher.started == 0,
            "constructed session observed or committed while disabled");
    if (pulses >= 3) {
      session->watch("flock");
      session->watch();
      progress = step::loading_a;
    }
    return true;
  case step::loading_a: {
    const auto result = safe_update();
    if (result.events.empty()) {
      return true;
    }
    require_applied(result);
    applied_a = result.events.front().generation_id;
    generation_a = session->current();
    require(generation_a && identify(*generation_a) == 1 && world.tick_count == 0, "A baseline");
    progress = step::running_a;
    break;
  }
  case step::running_a:
    require(safe_update().events.empty(), "A was applied twice");
    // Drain A's last manifest before publishing B, distinguishing a new fetch
    // while paused from a permitted late completion of an earlier request.
    if (world.tick_count >= 3 && fetcher.started == fetcher.completed) {
      pause();
      control("/publish-b");
      progress = step::published_b;
    }
    break;
  case step::published_b:
    require_paused();
    require(accepted == 1 && loader.completed == 1, "B was prepared without resuming observation");
    if (control_done() && pulses >= paused_pulse + 3) {
      session->watch();
      progress = step::loading_b;
    }
    break;
  case step::loading_b:
    // No update() here: observation and preparation must progress on their own.
    require(session->current() == generation_a && loader.completed == 1,
            "B escaped its download gate or changed the active module");
    if (accepted == 2) {
      pause();
      control("/release-b");
      progress = step::paused_b;
    }
    break;
  case step::paused_b:
    require_paused();
    if (loader.completed == 2 && ++ready_frames >= 3 && pulses >= paused_pulse + 3 &&
        control_done()) {
      session->watch("flock");
      // Commit retained work immediately, before a new timer can poll again.
      const auto result = safe_update();
      require_applied(result);
      applied_b = result.events.front().generation_id;
      generation_b = session->current();
      require(generation_b && generation_b != generation_a && identify(*generation_b) == 2 &&
                  applied_b != applied_a && fetcher.started == paused_fetches,
              "resume lost prepared B or replayed A");
      require(generation_b->entry("update_world") != generation_a->entry("update_world"),
              "B reused A's behavior entry");
      progress = step::running_b;
    }
    break;
  case step::running_b:
    require(safe_update().events.empty(), "B was applied twice");
    if (b_frames >= 3) {
      control("/publish-c");
      progress = step::rejecting_c;
    }
    break;
  case step::rejecting_c: {
    const auto result = safe_update();
    if (!result.events.empty()) {
      require(result.events.size() == 1, "C produced multiple transactions");
      const auto& event = result.events.front();
      require(event.status == update_status::rejected &&
                  event.code == candidate_error::incompatible &&
                  event.redirected_entry_count == 0 && !event.message.empty() &&
                  !event.generation_id.empty() && event.generation_id != applied_a &&
                  event.generation_id != applied_b,
              "C did not report an incompatible-generation rejection");
      rejected_ignored = ignored;
      rejected_tick = world.tick_count;
      progress = step::rejected_c;
    }
    require(session->current() == generation_b, "C changed the active generation");
    break;
  }
  case step::rejected_c:
    require(safe_update().events.empty() && session->current() == generation_b,
            "rejected generation replayed or replaced B");
    if (control_done() && ignored >= rejected_ignored + 2 &&
        world.tick_count >= rejected_tick + 3) {
      require(accepted == 3 && loader.completed == 3 && identify(*generation_a) == 1,
              "duplicate offers reloaded code or invalidated A");
      session->unwatch();
      pulse.reset();
      report(1, "CMake publications preserved the browser world across watch, late completion, "
                "resume and rejection");
      return false;
    }
    break;
  }
  advance_world();
  return true;
}

} // namespace

int main() {
  session = std::make_unique<reload_session>(
      loader, fetcher, scheduler, "offers/latest", "flock", [](const offer_event& event) {
        if (event.kind == offer_event_kind::offer_accepted) {
          ++accepted;
        } else if (event.kind == offer_event_kind::offer_ignored) {
          ++ignored;
        } else {
          require(false, event.message.c_str());
        }
      });
  pulse = scheduler.repeat([] { ++pulses; });
  emscripten_request_animation_frame_loop(frame, nullptr);
  emscripten_exit_with_live_runtime();
}
