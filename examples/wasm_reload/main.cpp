#include "contract.hpp"

#include <backends/wasm/emscripten_loader.hpp>
#include <backends/wasm/session.hpp>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <memory>
#include <string>
#include <string_view>

namespace {

demo::world_state world{0, 60.0f, 240.0f, 4.2f, -2.6f, 0};
demo::world_state* const the_world = &world;

neko::wasm::emscripten_loader loader;
neko::wasm::emscripten_manifest_fetcher fetcher;
std::unique_ptr<neko::wasm::reload_session> session;

EM_JS(void, draw_tick, (double x, double y, int behavior), {
  const canvas = document.getElementById('world');
  const ctx = canvas.getContext('2d');
  const colors = {1 : '#59a7ff', 2 : '#ffb347', 3 : '#7ee081'};
  ctx.fillStyle = colors[behavior] || '#888888';
  ctx.fillRect(x - 2, y - 2, 5, 5);
});

EM_JS(void, show_hud, (int behavior, unsigned ticks, int applied, int rejected, const char* note), {
  document.getElementById('hud').textContent = 'behavior ' + behavior + ' · tick ' + ticks +
                                               ' · applied ' + applied + ' · rejected ' + rejected +
                                               ' · ' + UTF8ToString(note);
});

EM_JS(void, note, (int ok, const char* message), {
  const log = document.getElementById('log');
  log.textContent += (ok ? '+ ' : '! ') + UTF8ToString(message) + '\n';
  log.scrollTop = log.scrollHeight;
});

EM_JS(int, smoke_mode, (), { return location.search.includes('smoke') ? 1 : 0; });

EM_JS(void, smoke_done, (int ok), {
  document.documentElement.setAttribute('data-smoke', ok ? 'ok' : 'fail');
  fetch('/smoke-result', {method : 'POST', body : ok ? 'ok' : 'fail'});
});

std::string describe(const neko::wasm::offer_event& event) {
  switch (event.kind) {
  case neko::wasm::offer_event_kind::offer_accepted:
    return "accepted offer: " + event.message;
  case neko::wasm::offer_event_kind::offer_ignored:
    return "ignored offer: " + event.message;
  case neko::wasm::offer_event_kind::offer_conflict:
    return "conflicting offer: " + event.message;
  case neko::wasm::offer_event_kind::manifest_invalid:
    return "invalid manifest: " + event.message;
  case neko::wasm::offer_event_kind::manifest_fetch_failed:
    return "manifest fetch failed: " + event.message;
  }
  return event.message;
}

unsigned applied_total = 0;
unsigned rejected_total = 0;
std::string last_note = "waiting for the first generation";

bool frame(double, void*) {
  for (const auto& event : session->update().events) {
    if (event.status == neko::wasm::update_status::applied) {
      ++applied_total;
      last_note = "applied generation " + event.generation_id + " (" +
                  std::to_string(event.redirected_entry_count) + " entries)";
      note(1, last_note.c_str());
    } else {
      ++rejected_total;
      last_note = "rejected generation " + event.generation_id + ": " + event.message;
      note(0, last_note.c_str());
    }
  }

  const auto snapshot = session->current();
  if (snapshot) {
    // One snapshot per frame: identity and update entries come from the
    // same generation even if another one activates mid-frame.
    const auto identity = reinterpret_cast<demo::identify_fn>(snapshot->entry("identify"));
    reinterpret_cast<demo::update_fn>(snapshot->entry("update_world"))(the_world);
    draw_tick(world.x, world.y, static_cast<int>(identity()));
  }
  show_hud(static_cast<int>(world.behavior), world.tick_count, static_cast<int>(applied_total),
           static_cast<int>(rejected_total), last_note.c_str());

  if (smoke_mode()) {
    if (applied_total >= 1 && world.tick_count > 20) {
      smoke_done(1);
      return false;
    }
    if (world.tick_count > 400) {
      smoke_done(0);
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  session = std::make_unique<neko::wasm::reload_session>(
      loader, fetcher, "offers/latest", demo::group_id,
      [](const neko::wasm::offer_event& event) { note(1, describe(event).c_str()); });
  emscripten_request_animation_frame_loop(frame, nullptr);
  return 0;
}
