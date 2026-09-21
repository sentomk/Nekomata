#pragma once

#include "candidate.hpp"
#include "manifest_fetcher.hpp"
#include "offer_poller.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace neko::wasm {

enum class update_status : std::uint8_t {
  applied,  ///< the transaction redirected the live entry set
  rejected, ///< the previous entry set stayed active; `message` says why
};

struct update_event {
  update_status status = update_status::applied;
  candidate_error code = candidate_error::none;
  std::string generation_id;
  std::size_t redirected_entry_count = 0;
  std::string message;
};

struct update_result {
  std::vector<update_event> events;
};

// The browser reload agent: one poller, one pending candidate, one active
// module, all on the application event loop. `update()` is the safe-point
// protocol — the caller drives it from a frame boundary and keeps
// reloadable code quiescent for the call. Each update polls the manifest
// URL (a no-op while a fetch is outstanding), reports a finished candidate
// exactly once as an applied or rejected transaction, and activates a ready
// candidate into the active entry set. The application owns the world; the
// session never builds code, runs behavior entries, or touches persistent
// state. Rejection classification reuses `candidate_error`.
class reload_session {
public:
  using diagnostics_callback = offer_poller::event_callback;

  // `expected_group` must be nonempty: one session watches one group, and
  // offers for any other group are ignored with a diagnostic.
  reload_session(module_loader& loader, manifest_fetcher& fetcher, std::string manifest_url,
                 std::string expected_group, diagnostics_callback on_diagnostics = {});
  ~reload_session();
  reload_session(const reload_session&) = delete;
  reload_session& operator=(const reload_session&) = delete;

  [[nodiscard]] update_result update();

  // The owning, immutable snapshot of the active entry set. A frame should
  // retain one snapshot and resolve every entry through it, so identity and
  // behavior entries belong to the same generation.
  [[nodiscard]] std::shared_ptr<const prepared_module> current() const noexcept;

private:
  offer_poller poller_;
  active_module active_;
  std::string reported_generation_;
};

} // namespace neko::wasm
