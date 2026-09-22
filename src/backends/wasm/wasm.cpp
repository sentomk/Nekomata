#include <neko/wasm.hpp>

#include "emscripten_loader.hpp"
#include "emscripten_scheduler.hpp"
#include "registration.hpp"
#include "session.hpp"
#include <runtime/session_access.hpp>
#include <stdexcept>

namespace neko::wasm {
namespace {
class browser_session final : public backend::session_driver {
public:
  browser_session() : session_(loader_, fetcher_, scheduler_, detail::discover_groups()) {}

  void watch() override { session_.watch(); }
  void watch(std::string_view group_id) override { session_.watch(group_id); }
  void unwatch() override { session_.unwatch(); }
  void unwatch(std::string_view group_id) override { session_.unwatch(group_id); }
  [[nodiscard]] update_result update() override { return session_.update(); }
  [[nodiscard]] session_snapshot snapshot() const override { return session_.snapshot(); }
  [[nodiscard]] std::shared_ptr<const prepared_module> current(std::string_view id) const {
    return session_.current(id);
  }

private:
  emscripten_loader loader_;
  emscripten_manifest_fetcher fetcher_;
  emscripten_poll_scheduler scheduler_;
  // Destroy the lifecycle before the adapters it borrows.
  managed_session session_;
};
} // namespace

std::unique_ptr<backend::session_driver> create_backend() {
  return std::make_unique<browser_session>();
}

entry_set acquire(const ::neko::reload_session& session, std::string_view group_id) {
  const auto* driver =
      dynamic_cast<const browser_session*>(backend::session_access::driver(session));
  if (!driver) {
    throw std::invalid_argument("wasm acquire: session does not own a browser backend");
  }
  entry_set result;
  result.module_ = driver->current(group_id);
  return result;
}
} // namespace neko::wasm
