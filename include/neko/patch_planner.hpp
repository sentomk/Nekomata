// patch_planner — from changed files to a concrete reload plan.
//
// Answers "what does this change break?": which translation units must be
// recompiled (dependency graph, seeded by compiler `.d` files) and which
// functions must be replaced once fresh object files exist.
//
// Inspiration: RuntimeCompiledCPlusPlus' dependency tracking.

#pragma once

#include <neko/types.hpp>

#include <vector>

namespace neko {

class patch_planner {
public:
    virtual ~patch_planner() = default;

    /// Compute the reload plan for a set of changed source files.
    virtual std::vector<patch_plan> plan(const change_set& changes) const = 0;
};

} // namespace neko
