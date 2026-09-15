// reload_session — the Live++-style in-process agent tick.
//
// Usage pattern:
//
//     neko::reload_session session{neko::elf::create_backend()};
//     session.watch("hot.new.o", "src/hot.cpp");
//     ... session.update() once per frame / loop iteration ...
//
// Trigger model: compilation is the caller's business. An individual object
// watch is claimed atomically when a complete file appears. A generation watch
// is claimed only when its manifest appears, after the producer has published
// its complete immutable object set. Half-written or unpublished objects are
// never picked up.
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <neko/runtime/fwd.hpp>

namespace neko {

/// A pluggable backend, assembled from its pieces by a backend factory
/// (e.g. neko::elf::create_backend()). Shared ownership: one implementation
/// object may serve several interface roles.
struct backend_bundle {
  std::shared_ptr<object_loader> loader;
  std::shared_ptr<symbol_provider> symbols;
  std::shared_ptr<state_manager> state;
  std::shared_ptr<code_substituter> substituter;
  std::shared_ptr<patch_planner> planner;
};

/// Watches an atomically published build-generation manifest. The manifest is
/// the ready marker: it names the generation, changed files, and the complete
/// object/source/build-information set. See docs/reload-model.md for its
/// on-disk format.
struct generation_watch {
  std::filesystem::path manifest_path;
};

class reload_session {
public:
  explicit reload_session(backend_bundle backends);
  ~reload_session();

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

  /// Watch complete build generations published at `manifest_path`. Unlike
  /// individual object watches, no object is claimed before the manifest is
  /// atomically published.
  void watch(generation_watch generation);

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

  /// Disable every managed group. Legacy object and manifest watches are not
  /// affected. Idempotent.
  void unwatch();

  /// Disable one managed group. Idempotent; an unknown ID is a configuration
  /// exception. Re-enabling a group resumes from its consumer cursor and
  /// never replays generations already observed. Disabling never restores
  /// applied machine code.
  void unwatch(std::string_view group_id);

  /// Literal overload for `unwatch(std::string_view)`.
  void unwatch(const char* group_id);

  /// Pick up the first ready generation, or batch ready individual object
  /// watches in registration order. Every object in the selected offer is
  /// prepared and validated before any live function entry is changed, then
  /// the offer is committed as one transaction. Returns true when that
  /// transaction was applied. Throws
  /// std::runtime_error if an object is rejected or commit fails; no entry
  /// changed by this call remains modified after a failed transaction.
  ///
  /// Before calling, the caller must ensure that no thread can enter or execute
  /// reloadable code, and must preserve that quiescent state until this method
  /// returns.
  bool update();

  /// Read-only session statistics for observers (TUI, logging, tests).
  struct stats {
    std::size_t applied = 0;  ///< reloads that took effect
    std::size_t rejected = 0; ///< offers refused (ambiguity, bad object, ...)
    std::string last_result;  ///< human-readable outcome of the last offer
    std::vector<std::string> watched_paths;
  };
  stats session_stats() const;

private:
  struct watched_object {
    std::filesystem::path object_path;
    std::filesystem::path source_path;
  };

  struct managed_group;

  struct prepared_reload;
  struct prepared_generation;
  std::unique_ptr<prepared_reload> try_prepare(const watched_object& watched);
  std::unique_ptr<prepared_generation> try_prepare(const generation_watch& watched);
  std::unique_ptr<prepared_reload> prepare_object(const std::vector<std::uint8_t>& bytes,
                                                  std::string watch_key,
                                                  const std::filesystem::path& source_path,
                                                  std::string build_information);
  void validate_generation(const prepared_generation& generation) const;
  void commit(const prepared_generation& generation);

  backend_bundle backends_;
  std::vector<watched_object> watched_;
  std::vector<generation_watch> generation_watches_;
  std::vector<std::unique_ptr<managed_group>> managed_groups_;

  void enable_managed_group(managed_group& group);
  [[nodiscard]] managed_group& find_managed_group(std::string_view group_id);
  std::unordered_map<std::string, std::unordered_set<std::string>>
      applied_generation_ids_by_manifest_;
  /// Functions redirected by the last fully-applied load of each watched
  /// object. Used to warn when a later load of that same object drops a
  /// function whose entry still jumps to stale arena code.
  std::unordered_map<std::string, std::unordered_map<std::string, std::uintptr_t>>
      last_redirected_by_object_;
  std::size_t applied_ = 0;
  std::size_t rejected_ = 0;
  std::string last_result_ = "no offers yet";
};

} // namespace neko
