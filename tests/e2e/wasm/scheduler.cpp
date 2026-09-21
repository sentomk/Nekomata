#include <backends/wasm/emscripten_scheduler.hpp>
#include <emscripten.h>

#include <cstdlib>
#include <memory>

namespace {

std::unique_ptr<neko::wasm::poll_subscription> one_shot;
std::unique_ptr<neko::wasm::poll_subscription> observer;
unsigned calls = 0;
unsigned observations = 0;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

} // namespace

int main() {
  neko::wasm::emscripten_poll_scheduler scheduler;
  auto cancelled = scheduler.repeat([] { require(false, "cancelled timer fired"); });
  cancelled.reset();
  one_shot = scheduler.repeat([] {
    ++calls;
    one_shot.reset(); // Destroying the executing subscription must be safe.
  });
  observer = scheduler.repeat([] {
    if (++observations == 4) {
      require(calls == 1, "timer did not fire once and stay cancelled");
      observer.reset();
      report(1, "event-loop scheduling survives scheduler destruction and cancels safely");
    }
  });
  require(calls == 0 && observations == 0, "timer ran synchronously");
  // Subscriptions own their timers independently of the scheduler object.
  return 0;
}
