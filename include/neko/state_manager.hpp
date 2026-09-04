// state_manager — keeping program state across reloads.
//
// Binds the mutable global/static variables of freshly compiled code onto
// the existing storage of the running process, so state survives the swap.
//
// Phase 1 validation: the draft's global_id (a numeric handle) had no
// producer — name-based lookup is what the loader actually needs, so
// map_global() now takes the symbol name. Phase 5 extends this interface
// with layout migration for changed types.

#pragma once

#include <string_view>

namespace neko {

class state_manager {
public:
  virtual ~state_manager() = default;

  /// Existing storage for a mutable global/static variable, or nullptr if
  /// the live process does not know it (fresh code introduced a new symbol).
  virtual void* map_global(std::string_view name) = 0;
};

} // namespace neko
