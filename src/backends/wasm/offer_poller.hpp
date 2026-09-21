#pragma once

#include "candidate.hpp"
#include "manifest_fetcher.hpp"

#include <protocol/wasm_offer.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace neko::wasm {

// Lexically joins the offer's relative artifact path to the manifest URL's
// directory. Manifest URLs carry no query parameters; artifact paths are
// relative portable paths by codec validation.
[[nodiscard]] std::string resolve_artifact_url(std::string_view manifest_url,
                                               std::string_view artifact_path);

enum class offer_event_kind : std::uint8_t {
  offer_accepted,        // superseding offer handed to a fresh candidate
  offer_ignored,         // stale or duplicate sequence
  offer_conflict,        // one sequence slot claimed by two identities
  manifest_invalid,      // fetched text rejected by the offer codec
  manifest_fetch_failed, // transport failure; polling continues
};

struct offer_event {
  offer_event_kind kind = offer_event_kind::offer_ignored;
  std::string message;
};

// Polls one stable manifest URL and turns superseding offers into loading
// candidates. Single-event-loop object with at most one manifest fetch
// outstanding; poll() is a no-op while one is pending, so the caller owns
// the polling cadence. Ordering follows `compare_wasm_offers` against the
// last accepted offer: a superseding offer replaces the pending candidate,
// and acceptance is delivery bookkeeping, not a promise the candidate will
// validate. A nonempty `expected_group` pins the watched group: offers for
// any other group are ignored without becoming the ordering reference. The
// application keeps the safe point and activates through `active_module`.
class offer_poller {
public:
  using event_callback = std::function<void(const offer_event&)>;

  offer_poller(module_loader& loader, manifest_fetcher& fetcher, std::string manifest_url,
               event_callback on_event = {}, std::string expected_group = {});
  ~offer_poller();
  offer_poller(const offer_poller&) = delete;
  offer_poller& operator=(const offer_poller&) = delete;

  void poll();

  // The candidate for the newest accepted offer, or null.
  [[nodiscard]] candidate* pending() noexcept;
  // The last accepted offer, or null.
  [[nodiscard]] const ::neko::detail::wasm_offer* accepted() const noexcept;

private:
  struct state;
  std::shared_ptr<state> state_;
};

} // namespace neko::wasm
