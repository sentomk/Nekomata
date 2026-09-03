// StateManager — keeping program state across reloads.
//
// Maps global/static variables of freshly compiled code onto the existing
// storage of the running process, so state survives the swap. Phase 5
// extends this to object layout migration and vtable updates.

#pragma once

#include <hlr/types.hpp>

namespace hlr {

class StateManager {
public:
    virtual ~StateManager() = default;

    /// Map a global/static variable of the new code onto existing storage.
    /// Returns the address the new code must use, or nullptr on failure.
    virtual void* mapGlobal(GlobalId global) = 0;
};

} // namespace hlr
