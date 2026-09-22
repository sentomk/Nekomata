#include "world.hpp"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <neko/session.hpp>
#include <neko/wasm.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using neko::group_state;
using neko::update_status;
using neko::wasm::entry_set;
using neko::wasm::group;

std::array<group, 2> groups{{
    {"alpha", "alpha/latest", "flock-test-v1", {"identify", "update_world"}},
    {"beta", "beta/latest", "flock-test-v1", {"identify", "update_world"}},
}};
std::array<world_state, 2> worlds{{{0, 100.0f, 1.0f, 0}, {0, 200.0f, 2.0f, 0}}};
const auto* const original_worlds = worlds.data();
std::array<entry_set, 2> baseline;
std::array<std::uint32_t, 2> identities{1, 1};
std::unique_ptr<neko::reload_session> session;
neko::session_snapshot saved_ready;
unsigned frames = 0;
unsigned stage = 0;

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

template <class action>
void expect_error(action invoke, std::string_view message) {
  try {
    invoke();
  } catch (const std::exception& error) {
    require(error.what() == message, "unexpected configuration error");
    return;
  }
  require(false, "configuration error was not reported");
}

std::uint32_t identity(const entry_set& entries) {
  return entries ? entries.get<std::uint32_t()>("identify")() : 0;
}

neko::session_snapshot inspect() {
  const auto before = worlds;
  const std::array active{identity(groups[0].acquire()), identity(groups[1].acquire())};
  auto result = session->snapshot();
  require(result.managed_groups.size() == 2 && result.managed_groups[0].group_id == "alpha" &&
              result.managed_groups[1].group_id == "beta" && result.watched_paths.empty(),
          "public snapshot lost the sorted group registry");
  require(worlds == before && active[0] == identity(groups[0].acquire()) &&
              active[1] == identity(groups[1].acquire()),
          "snapshot changed state or active code");
  return result;
}

neko::update_result safe_update() {
  const auto before = worlds;
  const auto previous = inspect();
  auto expected = previous;
  const std::array active{identity(groups[0].acquire()), identity(groups[1].acquire())};
  const auto result = session->update();
  require(worlds == before && worlds.data() == original_worlds, "update changed persistent worlds");
  std::array<bool, 2> applied{};
  std::string previous_id;
  for (const auto& event : result.events) {
    require((event.group_id == "alpha" || event.group_id == "beta") &&
                (previous_id.empty() || previous_id < event.group_id),
            "unexpected event order");
    previous_id = event.group_id;
    const auto index = event.group_id == "alpha" ? 0 : 1;
    require(previous.managed_groups[index].enabled && !event.generation_id.empty(),
            "disabled or unidentified transaction");
    if (event.status == update_status::applied) {
      require(event.code == neko::reload_error_code::none && event.redirected_function_count == 2 &&
                  event.message.empty(),
              "invalid applied event");
      applied[index] = true;
      ++expected.applied;
      expected.managed_groups[index].last_applied_generation = event.generation_id;
      expected.last_result = "applied generation '" + event.generation_id + "': 2 function(s)";
    } else {
      require(event.code == neko::reload_error_code::incompatible &&
                  event.redirected_function_count == 0 && !event.message.empty(),
              "invalid rejected event");
      ++expected.rejected;
      expected.last_result = event.message;
    }
  }
  const auto after = inspect();
  require(after.applied == expected.applied && after.rejected == expected.rejected &&
              after.last_result == expected.last_result &&
              result.any_applied() == (applied[0] || applied[1]),
          "history disagrees with events");
  for (std::size_t i = 0; i < groups.size(); ++i) {
    require(after.managed_groups[i].observed_sequence ==
                    previous.managed_groups[i].observed_sequence &&
                after.managed_groups[i].enabled == previous.managed_groups[i].enabled &&
                after.managed_groups[i].last_applied_generation ==
                    expected.managed_groups[i].last_applied_generation,
            "update changed observation or another group's history");
    require(applied[i] || identity(groups[i].acquire()) == active[i], "non-applied code changed");
  }
  return result;
}

void advance_worlds() {
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const auto entries = groups[i].acquire();
    require(identity(entries) == identities[i], "wrong active generation");
    const auto before = worlds;
    entries.get<void(world_state*)>("update_world")(&worlds[i]);
    const auto velocity =
        identities[i] == 2 && before[i].position > 10.0f ? -1.0f : before[i].velocity;
    require(worlds[i] == world_state{before[i].tick_count + 1, before[i].position + velocity,
                                     velocity, identities[i]} &&
                worlds[1 - i] == before[1 - i],
            "behavior lost state continuity or changed the other world");
  }
}

void require_single(const neko::update_result& result, std::string_view id) {
  require(result.events.size() == 1 && result.events[0].group_id == id && result.any_applied(),
          "expected one activation");
}

EM_BOOL frame(double, void*) {
  const auto snapshot = inspect();
  const auto& alpha = snapshot.managed_groups[0];
  const auto& beta = snapshot.managed_groups[1];
  switch (stage) {
  case 0:
    require(!alpha.enabled && !beta.enabled && alpha.observed_sequence == 0 &&
                beta.observed_sequence == 0 && safe_update().events.empty(),
            "initial session observed while disabled");
    if (++frames == 3) {
      session->watch();
      session->watch("alpha");
      stage = 1;
    }
    break;
  case 1:
    require(!groups[0].acquire() && !groups[1].acquire(), "preparation activated code");
    if (alpha.state == group_state::ready && beta.state == group_state::ready) {
      require(safe_update().events.size() == 2, "baseline did not apply both groups");
      baseline = {groups[0].acquire(), groups[1].acquire()};
      // Moving the public facade must preserve its live driver and group handles.
      auto moved = std::move(*session);
      *session = std::move(moved);
      frames = 0;
      stage = 2;
    }
    break;
  case 2:
    if (++frames == 3) {
      session->unwatch("alpha");
      session->unwatch("alpha");
      control("/publish-second");
      stage = 3;
    }
    break;
  case 3:
    // A manifest already in flight at unwatch may advance the paused cursor.
    require(!alpha.enabled && identity(groups[0].acquire()) == 1,
            "paused alpha activated a generation");
    if (control_done() && beta.observed_sequence == 2 && beta.state == group_state::ready) {
      require_single(safe_update(), "beta");
      identities[1] = 2;
      session->watch("alpha");
      stage = 4;
    }
    break;
  case 4:
    if (alpha.observed_sequence == 2) {
      require(alpha.state == group_state::preparing, "gated artifact completed too early");
      session->unwatch("alpha");
      control("/release-alpha");
      frames = 0;
      stage = 5;
    }
    break;
  case 5:
    require(!alpha.enabled && safe_update().events.empty(), "paused work was consumed");
    if (control_done() && alpha.state == group_state::ready && ++frames == 3) {
      saved_ready = snapshot;
      session->watch("alpha");
      require_single(safe_update(), "alpha");
      identities[0] = 2;
      control("/publish-third");
      stage = 6;
    }
    break;
  case 6:
    if (control_done() && alpha.observed_sequence == 3 && beta.observed_sequence == 3 &&
        alpha.state == group_state::failed && beta.state == group_state::ready) {
      const auto result = safe_update();
      require(result.events.size() == 2 && result.events[0].status == update_status::rejected &&
                  result.events[1].status == update_status::applied,
              "mixed outcomes lost group isolation");
      identities[1] = 3;
      frames = 0;
      stage = 7;
    }
    break;
  case 7:
    require(safe_update().events.empty() && snapshot.applied == 5 && snapshot.rejected == 1,
            "duplicate offer replayed a transaction");
    if (++frames == 30) {
      session->unwatch();
      require(!inspect().managed_groups[0].enabled && !inspect().managed_groups[1].enabled,
              "unwatch did not pause both groups");
      require(saved_ready.managed_groups[0].state == group_state::ready &&
                  !saved_ready.managed_groups[0].enabled && saved_ready.applied == 3,
              "saved observation snapshot changed");
      session.reset();
      advance_worlds();
      require(identity(baseline[0]) == 1 && identity(baseline[1]) == 1,
              "old entries did not survive session destruction");
      report(1, "installed public factory: two streams, pause/resume, mixed rejection, persistent "
                "worlds and owning entries passed");
      return EM_FALSE;
    }
    break;
  }
  if (groups[0].acquire() && groups[1].acquire()) {
    advance_worlds();
  }
  return EM_TRUE;
}
} // namespace

int main() {
  expect_error([] { static_cast<void>(neko::wasm::create_backend({groups[0], groups[0]})); },
               "wasm group: registration already belongs to a session");
  {
    neko::reload_session empty{neko::wasm::create_backend({})};
    require(empty.snapshot().managed_groups.empty() && empty.update().events.empty(),
            "empty registry not empty");
    expect_error([&] { empty.watch(); },
                 "reload_session: no registered reload groups; nothing to watch");
  }
  session =
      std::make_unique<neko::reload_session>(neko::wasm::create_backend({groups[1], groups[0]}));
  expect_error([] { session->watch("unknown"); }, "reload_session: unknown reload group 'unknown'");
  expect_error([] { session->unwatch("unknown"); },
               "reload_session: unknown reload group 'unknown'");
  expect_error([] { session->watch(std::filesystem::path{"hot.o"}); },
               "reload_session: object watches are not supported by this backend");
  expect_error([] { session->watch(std::filesystem::path{"hot.o"}, "hot.cpp"); },
               "reload_session: object watches are not supported by this backend");
  emscripten_request_animation_frame_loop(frame, nullptr);
  return 0;
}
