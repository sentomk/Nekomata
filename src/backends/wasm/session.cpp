#include "session.hpp"

#include <stdexcept>
#include <utility>

namespace neko::wasm {
namespace {

std::string require_group(std::string group) {
  if (group.empty()) {
    throw std::runtime_error("reload_session: expected group must not be empty");
  }
  return group;
}

} // namespace

struct reload_session::observation {
  std::shared_ptr<offer_poller> poller;
};

reload_session::reload_session(module_loader& loader, manifest_fetcher& fetcher,
                               poll_scheduler& scheduler, std::string manifest_url,
                               std::string expected_group, diagnostics_callback on_diagnostics)
    : scheduler_(scheduler), group_id_(require_group(std::move(expected_group))),
      poller_(std::make_shared<offer_poller>(loader, fetcher, std::move(manifest_url),
                                             std::move(on_diagnostics), group_id_)) {}

reload_session::~reload_session() {
  unwatch();
}

void reload_session::watch() {
  if (observation_) {
    return;
  }
  auto observed = std::make_shared<observation>(observation{poller_});
  auto subscription = scheduler_.repeat([weak = std::weak_ptr<observation>{observed}] {
    if (const auto live = weak.lock()) {
      live->poller->poll();
    }
  });
  if (!subscription) {
    throw std::runtime_error("reload_session: scheduler returned no subscription");
  }
  observation_ = std::move(observed);
  subscription_ = std::move(subscription);
}

void reload_session::require_known_group(std::string_view group_id) const {
  if (group_id != group_id_) {
    throw std::runtime_error("reload_session: unknown reload group '" + std::string{group_id} +
                             "'");
  }
}

void reload_session::watch(std::string_view group_id) {
  require_known_group(group_id);
  watch();
}

void reload_session::unwatch() {
  // Invalidate this enable cycle before cancelling its timer. A callback
  // queued before disable cannot become valid again after a later watch().
  observation_.reset();
  subscription_.reset();
}

void reload_session::unwatch(std::string_view group_id) {
  require_known_group(group_id);
  unwatch();
}

update_result reload_session::update() {
  update_result result;
  if (!observation_) {
    return result;
  }
  candidate* pending = poller_->pending();
  const auto* accepted = poller_->accepted();
  if (pending == nullptr || accepted == nullptr ||
      accepted->generation_id == reported_generation_) {
    return result;
  }

  update_event event;
  event.generation_id = accepted->generation_id;
  switch (pending->status()) {
  case candidate_status::ready:
    // The caller keeps reloadable code quiescent for this call; activation
    // itself performs no allocation, validation, or behavior invocation.
    if (active_.activate(*pending)) {
      event.status = update_status::applied;
      event.redirected_entry_count = active_.current()->entry_count();
    } else {
      event.status = update_status::rejected;
      event.code = candidate_error::invalid_contract;
      event.message = "activation refused a ready candidate";
    }
    break;
  case candidate_status::rejected:
    event.status = update_status::rejected;
    event.code = pending->error();
    event.message = std::string{pending->message()};
    break;
  case candidate_status::loading:
  case candidate_status::cancelled:
  case candidate_status::activated:
    return result;
  }

  reported_generation_ = accepted->generation_id;
  result.events.push_back(std::move(event));
  return result;
}

std::shared_ptr<const prepared_module> reload_session::current() const noexcept {
  return active_.current();
}

} // namespace neko::wasm
