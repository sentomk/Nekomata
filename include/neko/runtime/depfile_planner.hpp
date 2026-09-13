// depfile_planner — dependency expansion from compiler-generated Make depfiles.

#pragma once

#include <neko/runtime/patch_planner.hpp>

#include <filesystem>
#include <vector>

namespace neko {

/// Associates one translation unit with its compiler-generated dependency
/// file. Relative paths in all three fields use working_directory as their
/// base. An empty working_directory means the process working directory at
/// construction time.
struct depfile_entry {
  std::filesystem::path translation_unit;
  std::filesystem::path dependency_file;
  std::filesystem::path working_directory;
};

/// Expands changed sources or headers to every affected translation unit using
/// GNU Make-compatible dependency files, such as GCC/Clang -MMD output.
///
/// Dependency files are read on each plan() call. The configured files must
/// remain the last complete dependency snapshot until the generation planned
/// from them has been consumed; a producer can then atomically promote the next
/// snapshot without rebuilding the planner. This ordering preserves the old
/// edge when an edit removes the include that triggered the rebuild.
///
/// Missing or malformed files throw std::runtime_error rather than returning
/// an incomplete plan. Relative paths in change_set use the process working
/// directory at plan() time.
class depfile_planner final : public patch_planner {
public:
  explicit depfile_planner(std::vector<depfile_entry> entries);

  std::vector<patch_plan> plan(const change_set& changes) const override;

private:
  std::vector<depfile_entry> entries_;
};

} // namespace neko
