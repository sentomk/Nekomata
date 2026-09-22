#include "session.hpp"
#include "session_error.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace neko::wasm {

struct reload_session::group {
  struct observation {
    std::shared_ptr<offer_poller> poller;
  };

  std::string id;
  std::shared_ptr<offer_poller> poller;
  std::shared_ptr<observation> observed;
  std::unique_ptr<poll_subscription> subscription;
  active_module active;
  std::string reported_generation;
  std::string last_applied_generation;

  void disable() {
    // Invalidate queued ticks before cancelling the timer, including on resume.
    observed.reset();
    subscription.reset();
  }
};

reload_session::reload_session(module_loader& loader, manifest_fetcher& fetcher,
                               poll_scheduler& scheduler, std::vector<group_registration> groups)
    : scheduler_(scheduler) {
  std::sort(groups.begin(), groups.end(),
            [](const auto& a, const auto& b) { return a.group_id < b.group_id; });
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (groups[i].group_id.empty()) {
      throw std::runtime_error("reload_session: expected group must not be empty");
    }
    if (i != 0 && groups[i - 1].group_id == groups[i].group_id) {
      throw std::runtime_error("reload_session: duplicate reload group '" + groups[i].group_id +
                               "'");
    }
  }
  for (auto& registration : groups) {
    auto value = std::make_unique<group>();
    value->id = std::move(registration.group_id);
    value->poller =
        std::make_shared<offer_poller>(loader, fetcher, std::move(registration.manifest_url),
                                       std::move(registration.on_diagnostics), value->id);
    groups_.push_back(std::move(value));
  }
}

reload_session::~reload_session() {
  unwatch();
}

void reload_session::enable(group& value) {
  if (value.observed) {
    return;
  }
  auto observed = std::make_shared<group::observation>(group::observation{value.poller});
  auto subscription = scheduler_.repeat([weak = std::weak_ptr<group::observation>{observed}] {
    if (const auto live = weak.lock()) {
      live->poller->poll();
    }
  });
  if (!subscription) {
    throw std::runtime_error("reload_session: scheduler returned no subscription");
  }
  value.observed = std::move(observed);
  value.subscription = std::move(subscription);
}

void reload_session::watch() {
  if (groups_.empty()) {
    throw std::runtime_error("reload_session: no registered reload groups; nothing to watch");
  }
  for (auto& value : groups_) {
    enable(*value);
  }
}

const reload_session::group& reload_session::find_group(std::string_view group_id) const {
  const auto found = std::lower_bound(groups_.begin(), groups_.end(), group_id,
                                      [](const auto& value, auto id) { return value->id < id; });
  if (found == groups_.end() || (*found)->id != group_id) {
    throw std::runtime_error("reload_session: unknown reload group '" + std::string{group_id} +
                             "'");
  }
  return **found;
}

reload_session::group& reload_session::find_group(std::string_view group_id) {
  return const_cast<group&>(std::as_const(*this).find_group(group_id));
}

void reload_session::watch(std::string_view group_id) {
  enable(find_group(group_id));
}

void reload_session::unwatch() {
  for (auto& value : groups_) {
    value->disable();
  }
}

void reload_session::unwatch(std::string_view group_id) {
  find_group(group_id).disable();
}

::neko::update_result reload_session::update() {
  struct transaction {
    group* target;
    candidate* pending;
    update_event event;
    std::string reported_generation;
    std::string applied_generation;
    std::string last_result;
    std::string refusal_result;
  };
  update_result result;
  std::vector<transaction> transactions;
  // Allocate every group's event and bookkeeping before any live entry changes.
  for (auto& value : groups_) {
    if (!value->observed) {
      continue;
    }
    auto* pending = value->poller->pending();
    const auto* accepted = value->poller->accepted();
    if (pending == nullptr || accepted == nullptr ||
        accepted->generation_id == value->reported_generation) {
      continue;
    }
    if (pending->status() != candidate_status::ready &&
        pending->status() != candidate_status::rejected) {
      continue;
    }
    transaction next{};
    next.target = value.get();
    next.pending = pending;
    next.event.group_id = value->id;
    next.event.generation_id = accepted->generation_id;
    next.reported_generation = accepted->generation_id;
    next.event.status = update_status::rejected;
    if (pending->status() == candidate_status::ready) {
      next.event.code = reload_error_code::object_rejected;
      next.event.message = "activation refused a ready candidate";
      next.refusal_result = next.event.message;
      next.applied_generation = accepted->generation_id;
      next.last_result = "applied generation '" + accepted->generation_id +
                         "': " + std::to_string(accepted->entries.size()) + " function(s)";
    } else {
      next.event.code = classify_candidate_error(pending->error());
      next.event.message = std::string{pending->message()};
      next.last_result = next.event.message;
    }
    transactions.push_back(std::move(next));
  }

  result.events.reserve(transactions.size());
  for (auto& next : transactions) {
    auto& value = *next.target;
    if (next.pending->status() == candidate_status::ready) {
      // The caller keeps reloadable code quiescent; activation does not allocate.
      if (value.active.activate(*next.pending)) {
        next.event.status = update_status::applied;
        next.event.code = reload_error_code::none;
        next.event.message.clear();
        next.event.redirected_function_count = value.active.current()->entry_count();
      } else {
        next.last_result = std::move(next.refusal_result);
      }
    }
    if (next.event.status == update_status::applied) {
      ++applied_;
      value.last_applied_generation = std::move(next.applied_generation);
    } else {
      ++rejected_;
    }
    last_result_ = std::move(next.last_result);
    value.reported_generation = std::move(next.reported_generation);
    result.events.push_back(std::move(next.event));
  }
  return result;
}

::neko::session_snapshot reload_session::snapshot() const {
  session_snapshot out;
  out.applied = applied_;
  out.rejected = rejected_;
  out.last_result = last_result_;
  for (const auto& value : groups_) {
    group_snapshot entry;
    entry.group_id = value->id;
    entry.enabled = value->observed != nullptr;
    entry.last_applied_generation = value->last_applied_generation;
    const auto& poller = std::as_const(*value->poller);
    if (const auto* accepted = poller.accepted()) {
      entry.observed_sequence = accepted->sequence;
    }
    const auto* pending = poller.pending();
    if (pending != nullptr && pending->status() == candidate_status::ready) {
      entry.state = group_state::ready;
    } else if (pending != nullptr && pending->status() == candidate_status::rejected) {
      entry.state = group_state::failed;
    } else {
      entry.state = entry.enabled ? group_state::preparing : group_state::idle;
    }
    out.managed_groups.push_back(std::move(entry));
  }
  return out;
}

std::shared_ptr<const prepared_module> reload_session::current(std::string_view group_id) const {
  return find_group(group_id).active.current();
}

} // namespace neko::wasm
