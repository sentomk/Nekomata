#include "session_impl.hpp"

#include "descriptor_discovery.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace neko::detail {

std::filesystem::path normalized_source_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
}

reload_error_code classify_stream_rejection(const std::string& message) {
  const std::string_view text = message;
  if (text.find("does not match descriptor") != std::string_view::npos) {
    return reload_error_code::incompatible;
  }
  if (text.find("digest mismatch") != std::string_view::npos ||
      text.find("is missing or not a regular file") != std::string_view::npos ||
      text.find("resolves outside the generation directory") != std::string_view::npos) {
    return reload_error_code::integrity;
  }
  return reload_error_code::invalid_artifact;
}

reload_error_code classify_transaction_rejection(const std::string& message) {
  const std::string_view text = message;
  if (text.rfind("reload rejected and rolled back", 0) == 0) {
    return reload_error_code::commit_failed;
  }
  if (text.find("object file is not ready") != std::string_view::npos) {
    return reload_error_code::integrity;
  }
  return reload_error_code::object_rejected;
}

} // namespace neko::detail

namespace neko {

std::vector<backend::patch_plan> trivial_planner::plan(const backend::change_set& changes) const {
  std::vector<backend::patch_plan> plans;
  plans.reserve(changes.changed_files.size());
  for (const auto& file : changes.changed_files) {
    backend::patch_plan plan;
    plan.translation_units.push_back(file);
    plans.push_back(std::move(plan));
  }
  return plans;
}

native_session::native_session(backend::bundle backends) : backends_(std::move(backends)) {
  if (!backends_.loader || !backends_.symbols || !backends_.state || !backends_.substituter) {
    throw std::runtime_error("reload_session: incomplete backend bundle");
  }
  if (!backends_.planner) {
    backends_.planner = std::make_unique<trivial_planner>();
  }

  // Descriptor discovery is a construction-time configuration step: corrupt
  // or conflicting embedded descriptors are configuration exceptions. The
  // groups stay disabled until watch() names them.
  for (auto& descriptor : detail::discover_embedded_descriptors()) {
    auto group = std::make_unique<managed_group>();
    group->descriptor = std::move(descriptor);
    managed_groups_.push_back(std::move(group));
  }
  std::sort(managed_groups_.begin(), managed_groups_.end(),
            [](const std::unique_ptr<managed_group>& a, const std::unique_ptr<managed_group>& b) {
              return a->descriptor.group_id < b->descriptor.group_id;
            });
}

native_session::~native_session() {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_running_ = false;
  }
  worker_wake_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  for (const auto& allocation : active_allocations_) {
    allocation->release_to_process();
  }
  for (const auto& allocation : active_state_allocations_) {
    allocation->release_to_process();
  }
}

void native_session::check_fatal_error() const {
  if (commit_poisoned_) {
    throw detail::fatal_reload_error(std::string{detail::incomplete_rollback_message});
  }
  check_fatal_worker_error();
}

void native_session::watch(std::filesystem::path object_path) {
  check_fatal_error();
  watched_.push_back({std::move(object_path), {}});
}

void native_session::watch(std::filesystem::path object_path,
                           const std::filesystem::path& source_path) {
  check_fatal_error();
  watched_.push_back({std::move(object_path), detail::normalized_source_path(source_path)});
}

void native_session::watch() {
  check_fatal_error();
  if (managed_groups_.empty()) {
    throw std::runtime_error(
        "reload_session: no embedded reload group descriptors; nothing to watch");
  }
  for (auto& group : managed_groups_) {
    enable_managed_group(*group);
  }
}

void native_session::watch(std::string_view group_id) {
  check_fatal_error();
  enable_managed_group(find_managed_group(group_id));
}

void native_session::unwatch() {
  check_fatal_error();
  for (auto& group : managed_groups_) {
    group->enabled = false;
  }
}

void native_session::unwatch(std::string_view group_id) {
  check_fatal_error();
  find_managed_group(group_id).enabled = false;
}

session_snapshot native_session::snapshot() const {
  check_fatal_error();
  std::lock_guard<std::mutex> lock(worker_mutex_);
  session_snapshot out;
  out.applied = applied_;
  out.rejected = rejected_;
  out.last_result = last_result_;
  for (const auto& watched : watched_) {
    out.watched_paths.push_back(watched.object_path.string());
  }
  for (const auto& group : managed_groups_) {
    group_snapshot entry;
    entry.group_id = group->descriptor.group_id;
    entry.enabled = group->enabled;
    if (group->prepared) {
      entry.state = group_state::ready;
    } else if (group->failed) {
      entry.state = group_state::failed;
    } else if (group->enabled) {
      entry.state = group_state::preparing; // the worker owns observation
    } else {
      entry.state = group_state::idle;
    }
    entry.observed_sequence =
        group->stream ? group->stream->cursor() : group->descriptor.baseline_sequence;
    entry.last_applied_generation = group->last_applied_generation;
    out.managed_groups.push_back(std::move(entry));
  }
  return out;
}

update_result native_session::update() {
  check_fatal_error();
  update_result result;
  const auto redirected = [](const prepared_generation& generation) {
    std::size_t count = 0;
    for (const auto& reload : generation.reloads) {
      count += reload->image.replacements.size();
    }
    return count;
  };

  // Managed groups first, each its own all-or-nothing transaction: one
  // group's rejection never prevents the others from applying. update()
  // only commits what the preparation worker already finished; it never
  // discovers, parses, or relocates here.
  {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    for (auto& group : managed_groups_) {
      if (!group->enabled) {
        continue;
      }
      if (group->prepared) {
        update_event event;
        event.group_id = group->descriptor.group_id;
        event.generation_id = group->prepared->id;
        try {
          const std::size_t count = redirected(*group->prepared);
          commit(*group->prepared);
          group->failed = false;
          group->last_applied_generation = group->prepared->id;
          group->prepared.reset();
          event.redirected_function_count = count;
          result.events.push_back(std::move(event));
        } catch (const detail::fatal_reload_error&) {
          throw;
        } catch (const std::exception& e) {
          group->prepared.reset();
          event.status = update_status::rejected;
          event.code = detail::classify_transaction_rejection(e.what());
          event.message = e.what();
          ++rejected_;
          last_result_ = e.what();
          group->failed = true;
          result.events.push_back(std::move(event));
        }
        continue;
      }
      if (!group->pending_rejection.empty()) {
        update_event event;
        event.status = update_status::rejected;
        event.group_id = group->descriptor.group_id;
        event.generation_id = group->pending_rejection_id;
        event.code = detail::classify_stream_rejection(group->pending_rejection);
        event.message = group->pending_rejection;
        ++rejected_;
        last_result_ = event.message;
        group->pending_rejection.clear();
        result.events.push_back(std::move(event));
      }
    }
    lock.unlock();
  }

  // The object watch surface keeps its one-transaction-per-call shape; its
  // rejections are values now, reported after the managed events.
  try {
    prepared_generation generation;
    for (const auto& watched : watched_) {
      if (auto prepared = try_prepare(watched)) {
        generation.reloads.push_back(std::move(prepared));
      }
    }
    if (!generation.reloads.empty()) {
      link_generation(generation);
      validate_generation(generation);
      const std::size_t count = redirected(generation);
      commit(generation);
      update_event event;
      event.redirected_function_count = count;
      result.events.push_back(std::move(event));
    }
  } catch (const detail::fatal_reload_error&) {
    throw;
  } catch (const std::exception& e) {
    ++rejected_;
    last_result_ = e.what();
    update_event event;
    event.status = update_status::rejected;
    event.code = detail::classify_transaction_rejection(e.what());
    event.message = e.what();
    result.events.push_back(std::move(event));
  }
  return result;
}

reload_session::reload_session(backend::bundle backends)
    : reload_session(std::make_unique<native_session>(std::move(backends))) {}

} // namespace neko
