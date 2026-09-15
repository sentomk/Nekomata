// The managed-group state machine and its background worker: discovery
// polling, preparation off the update() path, and enable/disable bookkeeping.
// The worker never writes live entries; update() commits.

#include "session_impl.hpp"

#include <base/file.hpp>
#include <neko/log.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace neko {

void reload_session::impl::start_worker() {
  std::lock_guard<std::mutex> lock(worker_mutex_);
  if (worker_running_ || worker_.joinable()) {
    return;
  }
  worker_running_ = true;
  worker_ = std::thread([this] { worker_loop(); });
}

void reload_session::impl::raise_fatal_worker_error(std::exception_ptr error) {
  std::lock_guard<std::mutex> lock(worker_mutex_);
  if (!fatal_worker_error_) {
    fatal_worker_error_ = std::move(error);
  }
  worker_running_ = false;
}

void reload_session::impl::check_fatal_worker_error() const {
  std::lock_guard<std::mutex> lock(worker_mutex_);
  if (fatal_worker_error_) {
    std::rethrow_exception(fatal_worker_error_);
  }
}

void reload_session::impl::worker_loop() {
  try {
    for (;;) {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_wake_.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return !worker_running_; });
      if (!worker_running_) {
        return;
      }
      // Preparation runs under the lock: it never writes live entries or
      // calls application code, so blocking snapshot() briefly is the whole
      // interaction it may have with the outside world.
      for (auto& group : managed_groups_) {
        if (group->enabled) {
          prepare_managed_group(*group);
        }
      }
    }
  } catch (...) {
    raise_fatal_worker_error(std::current_exception());
  }
}

void reload_session::impl::prepare_managed_group(managed_group& group) {
  const auto observation = group.stream->poll();
  if (observation.status == detail::stream_status::idle) {
    return;
  }
  if (observation.status == detail::stream_status::rejected) {
    group.pending_rejection = observation.message;
    group.pending_rejection_sequence = observation.sequence;
    group.pending_rejection_id = observation.generation_id;
    group.failed = true;
    group.prepared.reset();
    return;
  }

  neko::log(neko::log_level::info, "generation '%s' covers %zu translation unit(s)\n",
            observation.generation_id.c_str(), observation.offer.members.size());
  try {
    auto generation = std::make_unique<prepared_generation>();
    generation->id = observation.generation_id;
    generation->manifest_key = "managed:" + group.descriptor.group_id;
    generation->reloads.reserve(observation.objects.size());
    for (std::size_t index = 0; index < observation.objects.size(); ++index) {
      const auto& member = observation.offer.members[index];
      generation->reloads.push_back(prepare_object(
          detail::read_required_bytes(observation.objects[index], "cannot open object file: "),
          group.descriptor.group_id + "/" + member.member,
          std::filesystem::path(member.source_identity), member.build_information));
    }
    link_generation(*generation);
    validate_generation(*generation);
    group.prepared = std::move(generation);
    group.failed = false;
    group.pending_rejection.clear();
  } catch (const std::exception& e) {
    // An artifact failure is a rejection value, not a worker fatality.
    group.pending_rejection = e.what();
    group.pending_rejection_sequence = observation.sequence;
    group.pending_rejection_id = observation.generation_id;
    group.failed = true;
    group.prepared.reset();
  }
}

void reload_session::impl::enable_managed_group(managed_group& group) {
  if (group.enabled) {
    return;
  }
  if (!group.descriptor.generation_root_hint || group.descriptor.generation_root_hint->empty()) {
    throw std::runtime_error("reload_session: reload group '" + group.descriptor.group_id +
                             "' has no generation root hint");
  }
  // The stream outlives disable/enable cycles: its cursor is the group's
  // consumption state and must not replay already-observed generations.
  if (!group.stream) {
    group.stream = std::make_unique<detail::generation_stream>(
        group.descriptor, *group.descriptor.generation_root_hint);
  }
  group.enabled = true;
  start_worker();
  worker_wake_.notify_all();
}

reload_session::impl::managed_group&
reload_session::impl::find_managed_group(std::string_view group_id) {
  const auto found = std::find_if(managed_groups_.begin(), managed_groups_.end(),
                                  [group_id](const std::unique_ptr<managed_group>& group) {
                                    return group->descriptor.group_id == group_id;
                                  });
  if (found == managed_groups_.end()) {
    throw std::runtime_error("reload_session: unknown reload group '" + std::string{group_id} +
                             "'");
  }
  return **found;
}

} // namespace neko
