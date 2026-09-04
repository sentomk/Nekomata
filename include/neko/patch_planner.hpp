// patch_planner — from changed files to a reload plan.
//
// Answers "what does this change break?": which translation units must be
// recompiled (dependency graph, seeded by compiler `.d` files — Phase 2)
// before a reload can be offered.
//
// Phase 1 validation: the interface shape held; the Phase 1 implementation
// is the trivial_planner in src/ (every changed file is one TU to rebuild).

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
