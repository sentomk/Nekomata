// state_manager — keeping program state across reloads.
//
// Binds the mutable global/static variables of freshly compiled code onto
// the existing storage of the running process, so state survives the swap.
//
// Validation: the draft's global_id (a numeric handle) had no
// producer — name-based lookup is what the loader actually needs, so
// map_global() now takes the symbol name. Layout migration (planned) extends this interface
// with layout migration for changed types.

#pragma once

#include <string_view>

namespace neko::backend {

class state_manager {
public:
  virtual ~state_manager() = default;

  /// Storage already present in the linked process for a mutable global or
  /// static variable. A nullptr means the process has no such symbol; a
  /// supporting object loader may allocate candidate storage for a new one.
  virtual void* map_global(std::string_view name) = 0;
};

} // namespace neko::backend
