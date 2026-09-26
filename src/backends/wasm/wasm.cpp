#include <neko/backend.hpp>
#include <neko/backend/session_driver.hpp>
#include <neko/wasm.hpp>

#include "emscripten_loader.hpp"
#include "emscripten_scheduler.hpp"
#include "registration.hpp"
#include "session.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace neko::wasm {
namespace {
bool browser_session_active = false;

// Claims the page's PLT before any other member is built, and releases it
// even when a later member's construction throws.
class single_session_guard {
public:
  single_session_guard() {
    if (browser_session_active) {
      throw std::invalid_argument("wasm backend: only one browser session may be active");
    }
    browser_session_active = true;
  }
  ~single_session_guard() { browser_session_active = false; }

  single_session_guard(const single_session_guard&) = delete;
  single_session_guard& operator=(const single_session_guard&) = delete;
};

class browser_session final : public backend::session_driver {
public:
  browser_session()
      : groups_(detail::discover_groups()), session_(loader_, fetcher_, scheduler_, groups_) {
    detail::validate_plt_slots(groups_);
  }

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
  single_session_guard guard_;
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
