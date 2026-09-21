// Object watch claiming and preparation: atomically claim a ready object
// and hand the loaded image to the shared transaction engine.

#include "session_impl.hpp"

#include <base/file.hpp>
#include <neko/log.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace neko {
namespace {

std::filesystem::path claimed_path_for(const std::filesystem::path& path) {
  return path.parent_path() / (path.filename().string() + ".claimed");
}

class claimed_files {
public:
  claimed_files() = default;

  ~claimed_files() {
    for (const auto& path : paths_) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  claimed_files(const claimed_files&) = delete;
  claimed_files& operator=(const claimed_files&) = delete;

  void add(std::filesystem::path path) { paths_.push_back(std::move(path)); }

private:
  std::vector<std::filesystem::path> paths_;
};

} // namespace

std::unique_ptr<reload_session::impl::prepared_reload>
reload_session::impl::try_prepare(const watched_object& watched) {
  const auto& path = watched.object_path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return nullptr;
  }

  // Claim the file atomically before touching it, so a writer mid-flight
  // cannot hand us a partial object. The rename is the "explicit confirm"
  // of the reload trigger model.
  const auto claimed_path = claimed_path_for(path);
  std::filesystem::rename(path, claimed_path, ec);
  if (ec) {
    return nullptr; // someone else claimed it first, or it vanished
  }
  claimed_files cleanup;
  cleanup.add(claimed_path);

  const auto bytes = detail::read_required_bytes(claimed_path, "cannot open object file: ");

  const auto source_path = watched.source_path.generic_string();

  // Plan first (the planner owns "what does this object cover"): it is the
  // seam the future dependency graph grows into.
  backend::change_set changes;
  changes.changed_files.push_back(source_path.empty() ? path.string() : source_path);
  const auto plans = backends_.planner->plan(changes);
  neko::log(neko::log_level::info, "plan covers %zu translation unit(s)\n", plans.size());
  return prepare_object(bytes, path.lexically_normal().generic_string(), watched.source_path, {});
}

} // namespace neko
