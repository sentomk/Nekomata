// The backend service provider interface: the seam a platform backend
// implements and the kernel consumes. Ordinary applications only need the
// factory from their platform header (e.g. <neko/elf.hpp>).
#pragma once

#include <memory>

#include <neko/backend/code_substituter.hpp>
#include <neko/backend/object_loader.hpp>
#include <neko/backend/patch_planner.hpp>
#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>

namespace neko::backend {

/// A pluggable backend, assembled from its pieces by a backend factory
/// (e.g. neko::elf::create_backend()). Shared ownership: one implementation
/// object may serve several interface roles.
struct bundle {
  std::shared_ptr<object_loader> loader;
  std::shared_ptr<symbol_provider> symbols;
  std::shared_ptr<state_manager> state;
  std::shared_ptr<code_substituter> substituter;
  std::shared_ptr<patch_planner> planner;
};

} // namespace neko::backend
