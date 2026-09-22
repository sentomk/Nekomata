#include <neko/wasm.hpp>

#include "emscripten_loader.hpp"
#include "emscripten_scheduler.hpp"
#include "session.hpp"

namespace neko::wasm {
namespace {
class browser_session final : public backend::session_driver {
public:
  explicit browser_session(const std::vector<group>& groups)
      : session_(loader_, fetcher_, scheduler_, bind_groups(groups)) {}

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
  reload_session session_;
};
} // namespace

std::unique_ptr<backend::session_driver> create_backend(std::vector<group> groups) {
  return std::make_unique<browser_session>(groups);
}
} // namespace neko::wasm
