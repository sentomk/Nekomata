// reload_session — the Live++-style in-process agent tick.
//
// Usage pattern (2–3 lines, matching the proposal's API choice):
//
//     neko::reload_session session{neko::elf::create_backend()};
//     session.watch("hot.new.o");
//     ... session.update() once per frame / loop iteration ...
//
// Trigger model (per proposal): compilation is the caller's business; a
// reload is applied only when a complete object file appears at a watched
// path. The file is claimed atomically (renamed away) before loading, so a
// half-written object is never picked up.
//
// Phase 1 constraints (deliberate pruning, proposal §阶段一): single
// thread, swap at a known-quiet point (between update() calls), no rollback.

#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <neko/code_substituter.hpp>
#include <neko/object_loader.hpp>
#include <neko/patch_planner.hpp>
#include <neko/state_manager.hpp>
#include <neko/symbol_provider.hpp>

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

private:
  bool try_load(const std::filesystem::path& path);

  backend_bundle backends_;
  std::vector<std::filesystem::path> watched_;
};

} // namespace neko
