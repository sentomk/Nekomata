// An ordinary release page: optimized and linked without assertions. Side
// modules must not import anything that only an assertion-enabled main module
// provides.
#include "public_project/plt.hpp"
#include "world.hpp"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <neko/session.hpp>
#include <neko/wasm.hpp>

#include <memory>

namespace {
std::unique_ptr<neko::reload_session> session;
world_state world{0, 100.0f, 1.0f, 0};
unsigned frames = 0;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });

bool frame(double, void*) {
  for (const auto& event : session->update().events) {
    if (event.status != neko::update_status::applied) {
      report(0, "the published generation was rejected");
      return false;
    }
  }
  if (public_plt::alpha::update) {
    // The first call into side-module code; a missing import traps here.
    public_plt::alpha::update_world(&world);
    const bool ok =
        world.tick_count == 1 && world.last_generation == 1 && public_plt::alpha::identify() == 1;
    report(ok, ok ? "release page ran the published generation"
                  : "the published generation behaved unexpectedly");
    return false;
  }
  if (++frames > 600) {
    report(0, "no generation applied");
    return false;
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
