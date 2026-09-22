// The browser hot-reload demo as a real consumer of the public API: one
// reload_session over the build-discovered groups, the same shape as the
// native backends. The one browser-specific step is acquiring the active
// entry set — there is no entry patching to redirect calls for us.
#include "contract.hpp"

#include <neko/wasm.hpp>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <memory>
#include <string>

namespace {

demo::world_state world{0, 60.0f, 240.0f, 4.2f, -2.6f, 0};
demo::world_state* const the_world = &world;

std::unique_ptr<neko::reload_session> session;
std::string last_note = "waiting for the first generation";

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

bool frame(double, void*) {
  for (const auto& event : session->update().events) {
    if (event.status == neko::update_status::applied) {
      last_note = "applied generation " + event.generation_id + " (" +
                  std::to_string(event.redirected_function_count) + " entries)";
      note(1, last_note.c_str());
    } else {
      last_note = "rejected generation " + event.generation_id + ": " + event.message;
      note(0, last_note.c_str());
    }
  }

  // One entry set per frame: identity and update entries come from the
  // same generation even if another one applies mid-frame.
  const auto page = neko::wasm::acquire(*session, demo::group_id);
  if (page) {
    page.get<demo::update_fn>("update_world")(the_world);
    draw_tick(world.x, world.y, static_cast<int>(page.get<demo::identify_fn>("identify")()));
  }

  const auto observed = session->snapshot();
  show_hud(static_cast<int>(world.behavior), world.tick_count, static_cast<int>(observed.applied),
           static_cast<int>(observed.rejected), last_note.c_str());

  if (smoke_mode()) {
    if (observed.applied >= 1 && world.tick_count > 20) {
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
  session = std::make_unique<neko::reload_session>(neko::wasm::create_backend());
  session->watch();
  emscripten_request_animation_frame_loop(frame, nullptr);
  return 0;
}
