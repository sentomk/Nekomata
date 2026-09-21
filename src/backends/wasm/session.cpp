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

reload_session::reload_session(module_loader& loader, manifest_fetcher& fetcher,
                               std::string manifest_url, std::string expected_group,
                               diagnostics_callback on_diagnostics)
    : poller_(loader, fetcher, std::move(manifest_url), std::move(on_diagnostics),
              require_group(std::move(expected_group))) {}

reload_session::~reload_session() = default;

update_result reload_session::update() {
  poller_.poll();

  update_result result;
  candidate* pending = poller_.pending();
  const auto* accepted = poller_.accepted();
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
