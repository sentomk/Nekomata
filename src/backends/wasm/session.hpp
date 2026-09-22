#pragma once

#include "candidate.hpp"
#include "group.hpp"
#include "manifest_fetcher.hpp"
#include "offer_poller.hpp"
#include "poll_scheduler.hpp"

#include <neko/session.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::wasm {

// Private page-side registration, independent of build-system discovery.
struct group_registration {
  std::string group_id;
  std::string manifest_url;
  offer_poller::event_callback on_diagnostics;
  std::shared_ptr<detail::group_binding> binding = {};
};

[[nodiscard]] std::vector<group_registration> bind_groups(const std::vector<group>& groups);

// The browser reload agent: each group owns a poller, pending candidate and
// active module on the application event loop. watch() starts preparation;
// update() only consumes prepared results at the application's safe point.
// The application owns the world and keeps reloadable code quiescent during
// update(). Transactions use the public session result vocabulary; candidate
// validation and the callable entry snapshot remain backend-private.
class reload_session {
public:
  // Group IDs must be nonempty and unique. All groups start disabled;
  // dependencies must outlive this session. Registration is fixed at construction.
  reload_session(module_loader& loader, manifest_fetcher& fetcher, poll_scheduler& scheduler,
                 std::vector<group_registration> groups);
  ~reload_session();
  reload_session(const reload_session&) = delete;
  reload_session& operator=(const reload_session&) = delete;

  // Known-group operations are idempotent; unknown IDs are configuration errors.
  // watch() requires at least one registration. If scheduling a group fails,
  // previously enabled groups stay enabled and watch() can be retried.
  void watch();
  void watch(std::string_view group_id);
  void unwatch();
  void unwatch(std::string_view group_id);

  // Pausing retains the consumer cursor and pending result, including an
  // in-flight completion. Disabled groups never commit or report a transaction.
  // Groups transact independently, in ascending group ID order.
  [[nodiscard]] ::neko::update_result update();

  // Pure observation by value: no polling, consumption or activation. A
  // disabled group can retain ready/failed state. Counters and last_result
  // describe consumed transactions, not preparation or transport diagnostics.
  [[nodiscard]] ::neko::session_snapshot snapshot() const;

  // The owning, immutable snapshot of the active entry set. A frame should
  // retain one snapshot and resolve every entry through it, so identity and
  // behavior entries belong to the same generation.
  [[nodiscard]] std::shared_ptr<const prepared_module> current(std::string_view group_id) const;

private:
  struct group;
  [[nodiscard]] group& find_group(std::string_view group_id);
  [[nodiscard]] const group& find_group(std::string_view group_id) const;
  void enable(group& value);
  poll_scheduler& scheduler_;
  std::vector<std::unique_ptr<group>> groups_;
  std::size_t applied_ = 0;
  std::size_t rejected_ = 0;
  std::string last_result_;
};

} // namespace neko::wasm
