// PatchPlanner — from changed files to a concrete reload plan.
//
// Answers "what does this change break?": which translation units must be
// recompiled (dependency graph, seeded by compiler `.d` files) and which
// functions must be replaced once fresh object files exist.
//
// Inspiration: RuntimeCompiledCPlusPlus' dependency tracking.

#pragma once

#include <hlr/types.hpp>

#include <vector>

namespace hlr {

class PatchPlanner {
public:
    virtual ~PatchPlanner() = default;

    /// Compute the reload plan for a set of changed source files.
    virtual std::vector<PatchPlan> plan(const ChangeSet& changes) const = 0;
};

} // namespace hlr
