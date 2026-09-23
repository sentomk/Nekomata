// Extension interface for implementing a backend. Ordinary applications only
// need the factory from their platform header (e.g. <neko/elf.hpp>).
#pragma once

#include <memory>

#include <neko/backend/code_substituter.hpp>
#include <neko/backend/object_loader.hpp>
#include <neko/backend/patch_planner.hpp>
#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/session.hpp>

namespace neko::backend {

/// A native backend assembled from its pieces before make_handle() wraps it.
/// Shared ownership lets one implementation object serve several roles.
struct bundle {
  std::shared_ptr<object_loader> loader;
  std::shared_ptr<symbol_provider> symbols;
  std::shared_ptr<state_manager> state;
  std::shared_ptr<code_substituter> substituter;
  std::shared_ptr<patch_planner> planner;
};

/// Wrap a native backend assembled by an extension or platform factory.
/// The returned handle transfers exclusive lifecycle ownership to a session.
backend_handle make_handle(bundle backends);

/// Wrap a managed backend's lifecycle implementation.
backend_handle make_handle(std::unique_ptr<session_driver> driver);

} // namespace neko::backend
