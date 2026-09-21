#include "offer_poller.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace neko::wasm {
namespace {

void deliver_event(const offer_poller::event_callback& callback, offer_event_kind kind,
                   std::string message) {
  if (callback) {
    callback(offer_event{kind, std::move(message)});
  }
}

} // namespace

struct offer_poller::state {
  module_loader* loader = nullptr;
  manifest_fetcher* fetcher = nullptr;
  std::string manifest_url;
  event_callback on_event;
  std::string expected_group;
  bool fetching = false;
  std::optional<::neko::detail::wasm_offer> last;
  std::unique_ptr<candidate> pending;

  void handle(manifest_text fetched) {
    if (!fetched.ok) {
      deliver_event(on_event, offer_event_kind::manifest_fetch_failed,
                    fetched.message.empty() ? "manifest fetch failed" : std::move(fetched.message));
      return;
    }
    ::neko::detail::wasm_offer offer;
    try {
      offer = ::neko::detail::parse_wasm_offer(fetched.text, manifest_url);
    } catch (const ::neko::detail::wasm_offer_error& error) {
      deliver_event(on_event, offer_event_kind::manifest_invalid, error.what());
      return;
    }

    if (!expected_group.empty() && offer.group_id != expected_group) {
      deliver_event(on_event, offer_event_kind::offer_ignored,
                    "offer group '" + offer.group_id + "' does not match watched group '" +
                        expected_group + "'");
      return;
    }

    const auto* previous = last.has_value() ? &*last : nullptr;
    switch (::neko::detail::compare_wasm_offers(offer, previous)) {
    case ::neko::detail::wasm_offer_ordering::stale:
      deliver_event(on_event, offer_event_kind::offer_ignored,
                    "stale sequence " + std::to_string(offer.sequence) + " behind " +
                        std::to_string(last->sequence));
      return;
    case ::neko::detail::wasm_offer_ordering::duplicate:
      deliver_event(on_event, offer_event_kind::offer_ignored,
                    "duplicate generation '" + offer.generation_id + "'");
      return;
    case ::neko::detail::wasm_offer_ordering::conflict:
      deliver_event(on_event, offer_event_kind::offer_conflict,
                    "sequence " + std::to_string(offer.sequence) + " claimed by generations '" +
                        last->generation_id + "' and '" + offer.generation_id + "'");
      return;
    case ::neko::detail::wasm_offer_ordering::supersede:
      break;
    }

    module_contract contract;
    contract.abi_id = offer.abi_id;
    contract.entries = offer.entries;
    contract.sha256 = offer.sha256;
    const std::string artifact_url = resolve_artifact_url(manifest_url, offer.artifact_path);
    const std::string accepted_id = offer.generation_id;
    const auto accepted_sequence = offer.sequence;

    // Replacing the pending candidate discards a late completion for the
    // superseded offer through ordinary candidate ownership.
    pending = std::make_unique<candidate>(*loader, artifact_url, std::move(contract));
    last = std::move(offer);
    deliver_event(on_event, offer_event_kind::offer_accepted,
                  "generation '" + accepted_id + "' at sequence " +
                      std::to_string(accepted_sequence));
  }
};

std::string resolve_artifact_url(std::string_view manifest_url, std::string_view artifact_path) {
  const auto slash = manifest_url.find_last_of('/');
  const std::string_view base =
      slash == std::string_view::npos ? std::string_view{} : manifest_url.substr(0, slash + 1);
  return std::string(base) + std::string(artifact_path);
}

offer_poller::offer_poller(module_loader& loader, manifest_fetcher& fetcher,
                           std::string manifest_url, event_callback on_event,
                           std::string expected_group)
    : state_(std::make_shared<state>()) {
  state_->loader = &loader;
  state_->fetcher = &fetcher;
  state_->manifest_url = std::move(manifest_url);
  state_->on_event = std::move(on_event);
  state_->expected_group = std::move(expected_group);
  if (state_->manifest_url.empty()) {
    throw std::runtime_error("offer_poller: manifest URL must not be empty");
  }
}

offer_poller::~offer_poller() = default;

void offer_poller::poll() {
  if (!state_ || state_->fetching) {
    return;
  }
  state_->fetching = true;
  state_->fetcher->fetch(state_->manifest_url,
                         [weak = std::weak_ptr<state>{state_}](manifest_text fetched) {
                           const auto live = weak.lock();
                           if (!live) {
                             return; // the poller is gone; the delivery needs no observer
                           }
                           live->fetching = false;
                           live->handle(std::move(fetched));
                         });
}

candidate* offer_poller::pending() noexcept {
  return state_ ? state_->pending.get() : nullptr;
}

const ::neko::detail::wasm_offer* offer_poller::accepted() const noexcept {
  return state_ && state_->last.has_value() ? &*state_->last : nullptr;
}

} // namespace neko::wasm
