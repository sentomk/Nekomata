#include "contract.hpp"

#include <backends/wasm/emscripten_loader.hpp>
#include <backends/wasm/emscripten_scheduler.hpp>
#include <backends/wasm/session.hpp>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using neko::group_state;
using neko::reload_error_code;
using neko::update_result;
using neko::update_status;
using namespace neko::wasm;

constexpr std::array<std::string_view, 2> ids{"alpha", "beta"};
std::array<world_state, 2> worlds{{{0, 100.0f, 1.0f, 0}, {0, 200.0f, 2.0f, 0}}};
const auto* const original_worlds = worlds.data();
std::array<unsigned, 2> accepted{};
std::array<unsigned, 2> ignored{};
std::array<unsigned, 2> final_ignored{};
std::array<std::uint32_t, 2> expected_identity{1, 1};
std::array<std::shared_ptr<const prepared_module>, 2> baseline;
std::array<std::shared_ptr<const prepared_module>, 2> second;
std::array<std::string, 2> second_ids;

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

std::size_t url_group(std::string_view url) {
  require(url.starts_with("alpha/") || url.starts_with("beta/"), "unexpected request group");
  return url.starts_with("alpha/") ? 0 : 1;
}

// Count requests without replacing the real transport, validation or loader.
class observed_fetcher final : public manifest_fetcher {
public:
  std::array<unsigned, 2> started{};
  std::array<unsigned, 2> completed{};

  void fetch(std::string url, completion complete) override {
    const auto index = url_group(url);
    ++started[index];
    inner_.fetch(std::move(url),
                 [this, index, complete = std::move(complete)](manifest_text result) {
                   complete(std::move(result));
                   ++completed[index];
                 });
  }

private:
  emscripten_manifest_fetcher inner_;
};

class observed_loader final : public module_loader {
public:
  std::array<unsigned, 2> completed{};

  void open(std::string path, std::string_view sha256, completion complete) override {
    const auto index = url_group(path);
    inner_.open(std::move(path), sha256,
                [this, index, complete = std::move(complete)](module_load_result result) {
                  complete(std::move(result));
                  ++completed[index];
                });
  }

private:
  emscripten_loader inner_;
};

observed_fetcher fetcher;
observed_loader loader;
emscripten_poll_scheduler scheduler;
std::unique_ptr<reload_session> session;
std::unique_ptr<poll_subscription> pulse;
unsigned pulses = 0;
unsigned paused_pulse = 0;
unsigned alpha_fetches = 0;
unsigned retained_frames = 0;
unsigned stage_tick = 0;
neko::session_snapshot retained;

enum class step {
  disabled,
  baseline,
  running,
  beta_update,
  alpha_loading,
  alpha_paused,
  both_updated,
  mixed_results,
  finished
};
step progress = step::disabled;

std::uint32_t identify(const prepared_module& module) {
  return reinterpret_cast<identify_fn>(module.entry("identify"))();
}

neko::session_snapshot inspect() {
  const auto before = worlds;
  const auto requests = fetcher.started;
  const auto alpha = session->current("alpha");
  const auto beta = session->current("beta");
  auto snapshot = session->snapshot();
  require(snapshot.managed_groups.size() == 2 && snapshot.managed_groups[0].group_id == "alpha" &&
              snapshot.managed_groups[1].group_id == "beta" && snapshot.watched_paths.empty(),
          "snapshot registry is not sorted or contains object watches");
  require(worlds == before && requests == fetcher.started && session->current("alpha") == alpha &&
              session->current("beta") == beta,
          "snapshot mutated observation, activation or world state");
  return snapshot;
}

update_result safe_update() {
  const auto before = worlds;
  auto expected = inspect();
  const auto alpha = session->current("alpha");
  const auto beta = session->current("beta");
  const auto requests = fetcher.started;
  auto result = session->update();
  require(worlds.data() == original_worlds && worlds == before && requests == fetcher.started,
          "update mutated persistent worlds or started observation");
  std::array<bool, 2> applied{};
  std::string previous_id;
  for (const auto& event : result.events) {
    require(event.group_id == "alpha" || event.group_id == "beta", "unknown transaction group");
    require(previous_id.empty() || previous_id < event.group_id, "unsorted or duplicate events");
    previous_id = event.group_id;
    const auto index = event.group_id == "alpha" ? 0 : 1;
    require(expected.managed_groups[index].enabled, "disabled group consumed a result");
    require(!event.generation_id.empty(), "transaction lost generation identity");
    if (event.status == update_status::applied) {
      require(event.code == reload_error_code::none && event.redirected_function_count == 2 &&
                  event.message.empty(),
              "applied event lost entry count or status");
      ++expected.applied;
      applied[index] = true;
      expected.managed_groups[index].last_applied_generation = event.generation_id;
      expected.last_result = "applied generation '" + event.generation_id + "': 2 function(s)";
    } else {
      require(event.code == reload_error_code::incompatible &&
                  event.redirected_function_count == 0 && !event.message.empty(),
              "wrong rejection classification");
      ++expected.rejected;
      expected.last_result = event.message;
    }
  }
  const auto after = inspect();
  require(after.applied == expected.applied && after.rejected == expected.rejected &&
              after.last_result == expected.last_result,
          "transaction history disagrees with events");
  for (std::size_t i = 0; i < ids.size(); ++i) {
    require(after.managed_groups[i].enabled == expected.managed_groups[i].enabled &&
                after.managed_groups[i].observed_sequence ==
                    expected.managed_groups[i].observed_sequence &&
                after.managed_groups[i].last_applied_generation ==
                    expected.managed_groups[i].last_applied_generation,
            "update changed another group's cursor or history");
    require(applied[i] || session->current(ids[i]) == (i == 0 ? alpha : beta),
            "non-applied group changed active code");
  }
  require(result.any_applied() == (applied[0] || applied[1]), "wrong any_applied result");
  return result;
}

void require_single(const update_result& result, std::string_view id) {
  require(result.events.size() == 1 && result.events[0].group_id == id &&
              result.events[0].status == update_status::applied,
          "expected one group's activation");
}

void advance_worlds() {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto active = session->current(ids[i]);
    require(active && identify(*active) == expected_identity[i], "wrong group's active behavior");
    const auto before = worlds;
    reinterpret_cast<update_fn>(active->entry("update_world"))(&worlds[i]);
    const auto velocity =
        expected_identity[i] == 2 && before[i].position > 10.0f ? -1.0f : before[i].velocity;
    require(
        worlds.data() == original_worlds && worlds[i].tick_count == before[i].tick_count + 1 &&
            worlds[i].last_generation == expected_identity[i] && worlds[i].velocity == velocity &&
            worlds[i].position == before[i].position + velocity && worlds[1 - i] == before[1 - i],
        "behavior reset, mixed or mutated the other world");
  }
}

bool frame(double, void*) {
  const auto snapshot = inspect();
  const auto& alpha = snapshot.managed_groups[0];
  const auto& beta = snapshot.managed_groups[1];
  switch (progress) {
  case step::disabled:
    require(!alpha.enabled && !beta.enabled && alpha.state == group_state::idle &&
                beta.state == group_state::idle && alpha.observed_sequence == 0 &&
                beta.observed_sequence == 0 && snapshot.applied == 0 && snapshot.rejected == 0 &&
                !session->current("alpha") && !session->current("beta") &&
                fetcher.started == std::array<unsigned, 2>{0, 0},
            "constructed groups did not start disabled");
    require(safe_update().events.empty(), "disabled startup committed code");
    if (pulses >= 3) {
      session->watch();
      session->watch("beta");
      progress = step::baseline;
    }
    return true;
  case step::baseline:
    if (alpha.state != group_state::ready || beta.state != group_state::ready) {
      return true;
    }
    require(alpha.observed_sequence == 1 && beta.observed_sequence == 1, "baseline cursors");
    {
      const auto result = safe_update();
      require(result.events.size() == 2 && result.events[0].status == update_status::applied &&
                  result.events[1].status == update_status::applied,
              "baseline did not apply both groups");
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      baseline[i] = session->current(ids[i]);
    }
    progress = step::running;
    break;
  case step::running:
    require(safe_update().events.empty(), "baseline replayed");
    if (worlds[0].tick_count >= 3 && fetcher.started[0] == fetcher.completed[0]) {
      session->unwatch("alpha");
      alpha_fetches = fetcher.started[0];
      paused_pulse = pulses;
      control("/publish-second");
      progress = step::beta_update;
    }
    break;
  case step::beta_update: {
    require(!alpha.enabled && alpha.observed_sequence == 1 && fetcher.started[0] == alpha_fetches &&
                accepted[0] == 1 && session->current("alpha") == baseline[0],
            "paused alpha observed or changed code");
    const auto result = safe_update();
    if (!result.events.empty()) {
      require(!second[1], "beta applied its second generation twice");
      require_single(result, "beta");
      second[1] = session->current("beta");
      second_ids[1] = result.events[0].generation_id;
      expected_identity[1] = 2;
      stage_tick = worlds[1].tick_count;
    }
    if (second[1] && worlds[1].tick_count >= stage_tick + 3 && control_done() &&
        pulses >= paused_pulse + 3) {
      require(beta.observed_sequence == 2 && loader.completed[0] == 1, "independent beta cursor");
      session->watch("alpha");
      progress = step::alpha_loading;
    }
    break;
  }
  case step::alpha_loading:
    require(session->current("alpha") == baseline[0] && loader.completed[0] == 1,
            "alpha escaped its download gate");
    require(safe_update().events.empty(), "loading alpha committed or beta replayed");
    if (alpha.observed_sequence == 2) {
      require(alpha.state == group_state::preparing, "gated alpha unexpectedly ready");
      session->unwatch("alpha");
      alpha_fetches = fetcher.started[0];
      paused_pulse = pulses;
      control("/release-alpha");
      progress = step::alpha_paused;
    }
    break;
  case step::alpha_paused:
    require(!alpha.enabled && beta.enabled && alpha.observed_sequence == 2 &&
                fetcher.started[0] == alpha_fetches && session->current("alpha") == baseline[0] &&
                session->current("beta") == second[1],
            "paused completion affected another group");
    require(safe_update().events.empty(), "paused alpha consumed retained result");
    require(snapshot.applied == 3 && snapshot.rejected == 0, "preparation changed history");
    if (alpha.state == group_state::ready && ++retained_frames >= 3 && pulses >= paused_pulse + 3 &&
        control_done()) {
      retained = inspect();
      session->watch("alpha");
      const auto result = safe_update();
      require_single(result, "alpha");
      require(fetcher.started[0] == alpha_fetches, "resume refetched retained alpha");
      second[0] = session->current("alpha");
      second_ids[0] = result.events[0].generation_id;
      expected_identity[0] = 2;
      stage_tick = worlds[0].tick_count;
      progress = step::both_updated;
    }
    break;
  case step::both_updated:
    require(safe_update().events.empty(), "second generation replayed");
    require(!retained.managed_groups[0].enabled &&
                retained.managed_groups[0].state == group_state::ready && retained.applied == 3,
            "saved snapshot changed after resume");
    if (worlds[0].tick_count >= stage_tick + 3) {
      control("/publish-third");
      progress = step::mixed_results;
    }
    break;
  case step::mixed_results:
    // Wait for both independently prepared results, without consuming either early.
    require(session->current("alpha") == second[0] && session->current("beta") == second[1],
            "preparation activated outside the safe point");
    if (alpha.state == group_state::failed && beta.state == group_state::ready && control_done()) {
      require(alpha.observed_sequence == 3 && beta.observed_sequence == 3 &&
                  snapshot.applied == 4 && snapshot.rejected == 0,
              "mixed preparation history");
      const auto result = safe_update();
      require(result.events.size() == 2 && result.events[0].group_id == "alpha" &&
                  result.events[0].status == update_status::rejected &&
                  result.events[1].group_id == "beta" &&
                  result.events[1].status == update_status::applied && result.any_applied(),
              "alpha rejection blocked beta or reordered events");
      require(result.events[0].generation_id != second_ids[0] &&
                  result.events[1].generation_id != second_ids[1],
              "third publication reused identity");
      expected_identity[1] = 3;
      final_ignored = ignored;
      stage_tick = worlds[1].tick_count;
      progress = step::finished;
    }
    break;
  case step::finished:
    require(safe_update().events.empty(), "mixed results replayed");
    require(snapshot.applied == 5 && snapshot.rejected == 1 && alpha.state == group_state::failed &&
                alpha.observed_sequence == 3 && beta.observed_sequence == 3 &&
                alpha.last_applied_generation == second_ids[0] &&
                beta.last_applied_generation != second_ids[1] &&
                session->current("alpha") == second[0],
            "mixed results corrupted group history");
    if (ignored[0] >= final_ignored[0] + 2 && ignored[1] >= final_ignored[1] + 2 &&
        worlds[1].tick_count >= stage_tick + 3) {
      require(accepted == std::array<unsigned, 2>{3, 3} && loader.completed == accepted,
              "duplicate offers reloaded code");
      require(identify(*baseline[0]) == 1 && identify(*baseline[1]) == 1 &&
                  identify(*second[0]) == 2 && identify(*second[1]) == 2,
              "old entry ownership lost");
      session->unwatch();
      const auto stopped = inspect();
      require(!stopped.managed_groups[0].enabled && !stopped.managed_groups[1].enabled &&
                  stopped.managed_groups[0].state == group_state::failed &&
                  stopped.managed_groups[1].state == group_state::idle,
              "all-group pause lost independent states");
      pulse.reset();
      report(1, "Two CMake streams preserved separate worlds through pause, late completion and "
                "mixed results");
      return false;
    }
    break;
  }
  advance_worlds();
  return true;
}

void diagnose(std::size_t index, const offer_event& event) {
  if (event.kind == offer_event_kind::offer_accepted) {
    ++accepted[index];
  } else if (event.kind == offer_event_kind::offer_ignored) {
    ++ignored[index];
  } else {
    require(false, event.message.c_str());
  }
}

} // namespace

int main() {
  // Deliberately reverse registration order; results and snapshots must still sort.
  session = std::make_unique<reload_session>(
      loader, fetcher, scheduler,
      std::vector<group_registration>{
          {"beta", "beta/latest", [](const offer_event& event) { diagnose(1, event); }},
          {"alpha", "alpha/latest", [](const offer_event& event) { diagnose(0, event); }}});
  pulse = scheduler.repeat([] { ++pulses; });
  emscripten_request_animation_frame_loop(frame, nullptr);
  emscripten_exit_with_live_runtime();
}
