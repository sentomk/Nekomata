// patch_planner — from changed files to a reload plan.
//
// Answers "what does this change break?": which translation units must be
// recompiled before a reload can be offered. reload_session falls back to
// treating every changed file as one translation unit when no planner is
// configured.

#pragma once

#include <neko/backend/types.hpp>

#include <vector>

namespace neko::backend {

class patch_planner {
public:
  virtual ~patch_planner() = default;

  /// Compute the reload plan for a set of changed source files.
  virtual std::vector<patch_plan> plan(const change_set& changes) const = 0;
};

} // namespace neko::backend
