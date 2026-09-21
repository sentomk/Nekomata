#pragma once

#include "candidate.hpp"
#include "manifest_fetcher.hpp"
#include "offer_poller.hpp"
#include "poll_scheduler.hpp"

#include <neko/session.hpp>

#include <memory>
#include <string>

namespace neko::wasm {

// The browser reload agent: one poller, one pending candidate, one active
// module, all on the application event loop. watch() starts preparation;
// update() only consumes prepared results at the application's safe point.
// The application owns the world and keeps reloadable code quiescent during
// update(). Transactions use the public session result vocabulary; candidate
// validation and the callable entry snapshot remain backend-private.
class reload_session {
public:
  using diagnostics_callback = offer_poller::event_callback;

  // `expected_group` must be nonempty: one session watches one group, and
  // offers for any other group are ignored with a diagnostic. The group
  // starts disabled. Dependencies must outlive this session.
  reload_session(module_loader& loader, manifest_fetcher& fetcher, poll_scheduler& scheduler,
                 std::string manifest_url, std::string expected_group,
                 diagnostics_callback on_diagnostics = {});
  ~reload_session();
  reload_session(const reload_session&) = delete;
  reload_session& operator=(const reload_session&) = delete;

  // The private session registers one group; the all-group forms select it.
  // Known-group operations are idempotent; unknown IDs are configuration errors.
  void watch();
  void watch(std::string_view group_id);
  void unwatch();
  void unwatch(std::string_view group_id);

  // Pausing retains the consumer cursor and pending result, including an
  // in-flight completion. Disabled groups never commit or report a transaction.
  [[nodiscard]] ::neko::update_result update();

  // The owning, immutable snapshot of the active entry set. A frame should
  // retain one snapshot and resolve every entry through it, so identity and
  // behavior entries belong to the same generation.
  [[nodiscard]] std::shared_ptr<const prepared_module> current() const noexcept;

private:
  void require_known_group(std::string_view group_id) const;
  struct observation;
  poll_scheduler& scheduler_;
  std::string group_id_;
  std::shared_ptr<offer_poller> poller_;
  std::shared_ptr<observation> observation_;
  std::unique_ptr<poll_subscription> subscription_;
  active_module active_;
  std::string reported_generation_;
};

} // namespace neko::wasm
