#include <emscripten.h>
#include <neko/wasm.hpp>
#include <stdexcept>
#include <string_view>

EM_JS(void, report, (int ok),
      { window.report_result({ok : !!ok, message : "empty generated registry"}); });

int main() {
  neko::reload_session session{neko::wasm::create_backend()};
  bool rejected = false;
  try {
    session.watch();
  } catch (const std::runtime_error& error) {
    rejected = std::string_view{error.what()} ==
               "reload_session: no registered reload groups; nothing to watch";
  }
  report(rejected && session.snapshot().managed_groups.empty() && session.update().events.empty());
}
