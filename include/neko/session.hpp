// reload_session — the Live++-style in-process agent tick.
//
// Usage pattern:
//
//     neko::reload_session session{neko::elf::create_backend()};
//     session.watch("hot.new.o");
//     ... session.update() once per frame / loop iteration ...
//
// Trigger model: compilation is the caller's business; a
// reload is applied only when a complete object file appears at a watched
// path. The file is claimed atomically (renamed away) before loading, so a
// half-written object is never picked up.
//
// Current constraints: single
// thread, swap at a known-quiet point (between update() calls), no rollback.

#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

class reload_session {
public:
  explicit reload_session(backend_bundle backends);

  /// Offer an object file path to watch. When a regular file appears there,
  /// it is claimed and loaded on the next update().
  void watch(std::filesystem::path object_path);

  /// Pick up any newly offered object file. Returns true when a reload was
  /// applied. Throws std::runtime_error on a failed reload attempt — the
  /// process keeps running the previously loaded code afterwards.
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
  bool try_load(const std::filesystem::path& path);

  backend_bundle backends_;
  std::vector<std::filesystem::path> watched_;
  /// Functions redirected by the last fully-applied load, by name. Used to
  /// warn when a later load drops a function whose entry still jumps to
  /// stale arena code.
  std::unordered_map<std::string, std::uintptr_t> last_redirected_;
  std::size_t applied_ = 0;
  std::size_t rejected_ = 0;
  std::string last_result_ = "no offers yet";
};

} // namespace neko
