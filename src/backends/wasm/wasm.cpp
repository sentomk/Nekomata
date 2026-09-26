#include <neko/backend.hpp>
#include <neko/backend/session_driver.hpp>
#include <neko/wasm.hpp>

#include "emscripten_loader.hpp"
#include "emscripten_scheduler.hpp"
#include "registration.hpp"
#include "session.hpp"
#include <stdexcept>
#include <vector>

namespace neko::wasm {
namespace {
bool browser_session_active = false;

class browser_session final : public backend::session_driver {
public:
  browser_session()
      : groups_(detail::discover_groups()), session_(loader_, fetcher_, scheduler_, groups_) {
    detail::validate_plt_slots(groups_);
    if (browser_session_active) {
      throw std::invalid_argument("wasm backend: only one browser session may be active");
    }
    browser_session_active = true;
  }

  ~browser_session() override { browser_session_active = false; }

  // Slots defined in translation units initialized after a global session
  // register late, so every observation start checks the complete set.
  void watch() override {
    detail::validate_plt_slots(groups_);
    session_.watch();
  }
  void watch(std::string_view group_id) override {
    detail::validate_plt_slots(groups_);
    session_.watch(group_id);
  }
  void unwatch() override { session_.unwatch(); }
  void unwatch(std::string_view group_id) override { session_.unwatch(group_id); }
  [[nodiscard]] update_result update() override { return session_.update(); }
  [[nodiscard]] session_snapshot snapshot() const override { return session_.snapshot(); }

private:
  std::vector<group_registration> groups_;
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
