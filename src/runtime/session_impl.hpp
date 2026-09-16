#pragma once

// reload_session's implementation core. The public header carries none of
// this machinery; the four session TUs split it by responsibility:
//   session.cpp            construction, watch surface, update() orchestration
//   prepare_generation.cpp legacy watch claiming and preparation
//   reload_transaction.cpp the zero-write/commit/rollback engine
//   managed_worker.cpp     the managed-group state machine and its worker

#include <neko/backend.hpp>
#include <neko/session.hpp>

#include "generation_stream.hpp"

#include <protocol/group_descriptor.hpp>

#include <neko/backend/code_substituter.hpp>
#include <neko/backend/object_loader.hpp>
#include <neko/backend/patch_planner.hpp>

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::detail {

inline constexpr std::string_view incomplete_rollback_message =
    "reload session is unusable: an entry patch failed and rollback could not restore every "
    "written entry";

class fatal_reload_error final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Lexically normalized, absolute form of a watched source path. Compared
/// without opening or canonicalizing the file.
[[nodiscard]] std::filesystem::path normalized_source_path(const std::filesystem::path& path);

// Transitional classification: stream rejections carry stable wording, and
// transaction rejections are prefixed by their phase. Stable per-reason codes
// replace the matching once the low-level layers carry typed errors outward.
[[nodiscard]] reload_error_code classify_stream_rejection(const std::string& message);
[[nodiscard]] reload_error_code classify_transaction_rejection(const std::string& message);

} // namespace neko::detail

namespace neko {

// Trivial planner: every changed file is one translation unit to rebuild.
// Real dependency graphs (compiler .d files) are planned.
class trivial_planner final : public backend::patch_planner {
public:
  std::vector<backend::patch_plan> plan(const backend::change_set& changes) const override;
};

class reload_session::impl {
public:
  explicit impl(backend::bundle backends);
  ~impl();

  impl(const impl&) = delete;
  impl& operator=(const impl&) = delete;

  struct watched_object {
    std::filesystem::path object_path;
    std::filesystem::path source_path;
  };

  struct prepared_reload {
    std::string watch_key;
    std::string build_information;
    backend::loaded_image image;
  };

  struct prepared_generation {
    std::string id;
    std::string manifest_key;
    std::vector<std::unique_ptr<prepared_reload>> reloads;
  };

  struct managed_group {
    detail::group_descriptor descriptor;
    std::unique_ptr<detail::generation_stream> stream;
    bool enabled = false;
    bool failed = false;
    std::string last_applied_generation;
    // Worker outcome awaiting the next update(): a prepared transaction, or a
    // rejection to report exactly once. A newer prepared generation supersedes
    // older uncommitted work.
    std::unique_ptr<prepared_generation> prepared;
    std::string pending_rejection;
    std::uint64_t pending_rejection_sequence = 0;
    std::string pending_rejection_id;
  };

  void watch(std::filesystem::path object_path);
  void watch(std::filesystem::path object_path, const std::filesystem::path& source_path);
  void watch(generation_watch generation);
  void watch();
  void watch(std::string_view group_id);
  void unwatch();
  void unwatch(std::string_view group_id);
  [[nodiscard]] update_result update();
  [[nodiscard]] session_snapshot snapshot() const;

  // reload_transaction.cpp — the shared engine both watch surfaces drive.
  [[nodiscard]] std::unique_ptr<prepared_reload>
  prepare_object(const std::vector<std::uint8_t>& bytes, std::string watch_key,
                 const std::filesystem::path& source_path, std::string build_information);
  void link_generation(prepared_generation& generation);
  void validate_generation(const prepared_generation& generation) const;
  void commit(prepared_generation& generation);

  // prepare_generation.cpp — legacy watch claiming and preparation.
  [[nodiscard]] std::unique_ptr<prepared_reload> try_prepare(const watched_object& watched);
  [[nodiscard]] std::unique_ptr<prepared_generation> try_prepare(const generation_watch& watched);

  // managed_worker.cpp — the managed-group state machine and its worker.
  void enable_managed_group(managed_group& group);
  [[nodiscard]] managed_group& find_managed_group(std::string_view group_id);
  void start_worker();
  void worker_loop();
  void prepare_managed_group(managed_group& group);
  void raise_fatal_worker_error(std::exception_ptr error);
  void check_fatal_error() const;
  void check_fatal_worker_error() const;

  backend::bundle backends_;
  std::vector<watched_object> watched_;
  std::vector<generation_watch> generation_watches_;
  std::vector<std::unique_ptr<managed_group>> managed_groups_;

  // Background preparation. The worker discovers offers, parses objects, and
  // builds transactions without touching live entries; only update() commits.
  // It starts with the first enabled group and joins at destruction. Public
  // calls stay externally serialized; the mutex only separates the worker
  // from those calls.
  mutable std::mutex worker_mutex_;
  std::condition_variable worker_wake_;
  std::thread worker_;
  bool worker_running_ = false;
  std::exception_ptr fatal_worker_error_;
  bool commit_poisoned_ = false;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      applied_generation_ids_by_manifest_;
  /// Functions redirected by the last fully-applied load of each watched
  /// object. Used to warn when a later load of that same object drops a
  /// function whose entry still jumps to stale arena code.
  std::unordered_map<std::string, std::unordered_map<std::string, std::uintptr_t>>
      last_redirected_by_object_;
  /// Every executable allocation reached by an installed redirect. Old
  /// generations remain live because a removed function may still point into
  /// one. The destructor transfers these mappings to process lifetime: entry
  /// redirects deliberately outlive the session object today.
  std::vector<backend::executable_allocation_ptr> active_allocations_;
  /// Writable storage introduced by committed generations follows the same
  /// lifetime rule as redirected code: the session retains it while active,
  /// then transfers it because installed redirects are not undone.
  std::vector<backend::writable_allocation_ptr> active_state_allocations_;
  std::size_t applied_ = 0;
  std::size_t rejected_ = 0;
  std::string last_result_ = "no offers yet";
};

} // namespace neko
