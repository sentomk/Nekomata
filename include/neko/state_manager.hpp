// state_manager — keeping program state across reloads.
//
// Maps global/static variables of freshly compiled code onto the existing
// storage of the running process, so state survives the swap. Phase 5
// extends this to object layout migration and vtable updates.

#pragma once

#include <neko/types.hpp>

namespace neko {

class state_manager {
public:
    virtual ~state_manager() = default;

    /// Map a global/static variable of the new code onto existing storage.
    /// Returns the address the new code must use, or nullptr on failure.
    virtual void* map_global(global_id global) = 0;
};

} // namespace neko
