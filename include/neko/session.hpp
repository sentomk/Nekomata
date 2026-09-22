// reload_session — the Live++-style in-process agent tick.
//
// Usage pattern:
//
//     neko::reload_session session{neko::elf::create_backend()};
//     session.watch("hot.new.o", "src/hot.cpp");
//     ... session.update() once per frame / loop iteration ...
//
// Trigger model: compilation is the caller's business. An individual object
// watch is claimed atomically when a complete file appears. Managed groups
// apply a generation only when its publisher marks it ready, after the
// complete immutable object set exists. Half-written or unpublished objects
// are never picked up.
//
// Threading model: reload_session performs no internal synchronization. The
// caller must serialize all member calls and establish a quiescent point for
// reloadable code before update(), keeping it quiescent until update() returns.
// Every complete offer handled by one update() is committed all-or-nothing.
// A failed commit restores entries already written for that transaction.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <neko/fwd.hpp>

namespace neko {

/// Outcome of one committed or rejected transaction inside an `update()` call.
enum class update_status : std::uint8_t {
  applied,  ///< the transaction redirected live entries
  rejected, ///< the old code stayed active; `message` and `code` say why
};

/// Stable rejection classification. The set grows conservatively: codes are
/// contracts, the human-readable message next to them is diagnostic.
enum class reload_error_code : std::uint8_t {
  none = 0,         ///< no error; the companion status is `applied`
  invalid_artifact, ///< malformed or incomplete published data
  incompatible,     ///< group, compatibility, or ABI identity mismatch
  integrity,        ///< missing object, digest mismatch, or unsafe path
  object_rejected,  ///< parse, symbol, relocation, or entry-check failure
  commit_failed,    ///< an entry write failed and was successfully rolled back
};

/// One transaction outcome. Managed reload groups carry their `group_id` and
/// `generation_id`; object watch transactions leave both empty.
struct update_event {
  update_status status = update_status::applied;
  reload_error_code code = reload_error_code::none;
  std::string group_id;
  std::string generation_id;
  std::string message;
  std::size_t redirected_function_count = 0;
};

/// The result of one `update()` call. An empty event list means nothing was
/// ready. Managed-group events come first in ascending `group_id` order; an
/// object watch transaction, when one applies, comes last.
struct update_result {
  std::vector<update_event> events;

  /// True when at least one transaction was applied.
  [[nodiscard]] bool any_applied() const {
    for (const auto& event : events) {
      if (event.status == update_status::applied) {
        return true;
      }
    }
    return false;
  }
};

/// Observation state of one managed group. An enabled group is `preparing`
/// while the background worker owns its observation, `ready` once a complete
/// transaction waits for the next update(), `failed` after its last observed
/// generation was rejected, and `idle` when disabled or nothing is enabled.
enum class group_state : std::uint8_t {
  idle,
  preparing,
  ready,
  failed,
};

/// Immutable per-group view for observers such as the TUI, logs, and tests.
struct group_snapshot {
  std::string group_id;
  bool enabled = false;
  group_state state = group_state::idle;
  std::uint64_t observed_sequence = 0; ///< the group's consumer cursor
  std::string last_applied_generation;
};

/// Immutable whole-session view. Never exposes references into mutable
/// session state.
struct session_snapshot {
  std::size_t applied = 0;
  std::size_t rejected = 0;
  std::string last_result;
  std::vector<group_snapshot> managed_groups; ///< ascending by group ID
  std::vector<std::string> watched_paths;
};

/// The reload agent. One session exclusively owns its platform lifecycle:
/// observation, preparation, and activation at the caller's safe point.
class reload_session {
public:
  explicit reload_session(backend::bundle backends);
  /// Own a platform lifecycle implementation supplied by its backend factory.
  /// A null driver is a configuration error.
  explicit reload_session(std::unique_ptr<backend::session_driver> driver);
  ~reload_session();

  reload_session(reload_session&&) noexcept;
  reload_session& operator=(reload_session&&) noexcept;

  reload_session(const reload_session&) = delete;
  reload_session& operator=(const reload_session&) = delete;

  /// Offer an object file path to watch. When a regular file appears there,
  /// it is claimed and loaded on the next update(). This name-only form can
  /// reload exported functions, but refuses to guess which file-static
  /// function to patch when a live function has the same name.
  void watch(std::filesystem::path object_path);

  /// Watch an object produced for `source_path`. The source identity is
  /// matched against the offline symbol manifest when file-static function
  /// names need disambiguation; it is supplied by the build integration, not
  /// inferred from the fresh object's changing symbol set. A relative path is
  /// made absolute when registered, then compared lexically (without opening
  /// or canonicalizing the source file).
  void watch(std::filesystem::path object_path, const std::filesystem::path& source_path);

  /// Enable every managed reload group embedded in this program, discovered
  /// from the linked descriptors at session construction. Throws a
  /// configuration exception when the executable embeds no group descriptor,
  /// or a group carries no generation-root hint. Idempotent.
  void watch();

  /// Enable one managed reload group by its group ID. Throws a configuration
  /// exception for an unknown ID. Idempotent.
  void watch(std::string_view group_id);

  /// Literal overload: without it, `watch("id")` would be ambiguous between
  /// the path and string_view forms. It retires together with the path
  /// watches it disambiguates.
  void watch(const char* group_id);

  /// Pause every managed group, retaining its cursor and pending result.
  /// Disabled groups start no new observation and do not commit or report
  /// pending results in update(). Already-started preparation may finish.
  /// Object watches are not affected. Idempotent.
  void unwatch();

  /// Disable one managed group. Idempotent; an unknown ID is a configuration
  /// exception. Re-enabling a group resumes from its consumer cursor and
  /// never replays generations already observed. Prepared work and unreported
  /// rejections survive the pause; after re-enabling, update() may consume
  /// them unless newer observations supersede them. Disabling never restores
  /// applied machine code. An enabled flag and a ready/failed snapshot state
  /// are independent: a disabled group can retain a pending result.
  void unwatch(std::string_view group_id);

  /// Literal overload for `unwatch(std::string_view)`.
  void unwatch(const char* group_id);

  /// Pick up every ready managed group and, when no managed transaction was
  /// rejected, the batched object watches.
  /// Each managed group is its own all-or-nothing transaction; one group's
  /// rejection does not prevent the others from applying. Artifact problems
  /// are rejected events, not exceptions: no entry changed for a rejected
  /// transaction remains modified. If an entry write fails and rollback
  /// cannot restore every earlier write, `update()` throws `std::runtime_error`
  /// and the session becomes unusable; later member calls throw the same
  /// error. This fatal case is not represented as a rejected event.
  ///
  /// Before calling, the caller must ensure that no thread can enter or execute
  /// reloadable code, and must preserve that quiescent state until this method
  /// returns.
  [[nodiscard]] update_result update();

  /// Immutable observation snapshot; the input for logs, tests, and the
  /// optional TUI.
  [[nodiscard]] session_snapshot snapshot() const;

private:
  std::unique_ptr<backend::session_driver> impl_;
};

} // namespace neko
