#include <neko/backend/session_driver.hpp>
#include <neko/wasm.hpp>

#include "emscripten_loader.hpp"
#include "emscripten_scheduler.hpp"
#include "registration.hpp"
#include "session.hpp"
#include <stdexcept>

namespace neko::wasm {
namespace {
bool browser_session_active = false;

class browser_session final : public backend::session_driver {
public:
  browser_session() : session_(loader_, fetcher_, scheduler_, detail::discover_groups()) {
    if (browser_session_active) {
      throw std::invalid_argument("wasm backend: only one browser session may be active");
    }
    browser_session_active = true;
  }

  ~browser_session() override { browser_session_active = false; }

  void watch() override { session_.watch(); }
  void watch(std::string_view group_id) override { session_.watch(group_id); }
  void unwatch() override { session_.unwatch(); }
  void unwatch(std::string_view group_id) override { session_.unwatch(group_id); }
  [[nodiscard]] update_result update() override { return session_.update(); }
  [[nodiscard]] session_snapshot snapshot() const override { return session_.snapshot(); }

private:
  emscripten_loader loader_;
  emscripten_manifest_fetcher fetcher_;
  emscripten_poll_scheduler scheduler_;
  // Destroy the lifecycle before the adapters it borrows.
  managed_session session_;
};
} // namespace

backend_handle create_backend() {
  return backend::make_handle(std::make_unique<browser_session>());
}
} // namespace neko::wasm
