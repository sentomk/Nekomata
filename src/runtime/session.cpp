#include <neko/session.hpp>

#include <neko/log.hpp>
#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/patch_planner.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace neko {
namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("cannot open object file: " + path.string());
  }
  const std::streamsize size = file.tellg();
  file.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    file.read(reinterpret_cast<char*>(bytes.data()), size);
  }
  return bytes;
}

} // namespace

// Phase 1 planner: every changed file is one translation unit to rebuild.
// Real dependency graphs (compiler .d files) arrive in Phase 2.
class trivial_planner final : public patch_planner {
public:
  std::vector<patch_plan> plan(const change_set& changes) const override {
    std::vector<patch_plan> plans;
    plans.reserve(changes.changed_files.size());
    for (const auto& file : changes.changed_files) {
      patch_plan plan;
      plan.translation_units.push_back(file);
      plans.push_back(std::move(plan));
    }
    return plans;
  }
};

reload_session::reload_session(backend_bundle backends) : backends_(std::move(backends)) {
  if (!backends_.loader || !backends_.symbols || !backends_.state || !backends_.substituter) {
    throw std::runtime_error("reload_session: incomplete backend bundle");
  }
  if (!backends_.planner) {
    backends_.planner = std::make_unique<trivial_planner>();
  }
}

void reload_session::watch(std::filesystem::path object_path) {
  watched_.push_back(std::move(object_path));
}

bool reload_session::update() {
  bool reloaded = false;
  for (const auto& path : watched_) {
    if (try_load(path)) {
      reloaded = true;
    }
  }
  return reloaded;
}

bool reload_session::try_load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return false;
  }

  // Claim the file atomically before touching it, so a writer mid-flight
  // cannot hand us a partial object. The rename is the "explicit confirm"
  // of the reload trigger model.
  const auto& claimed = path;
  const auto claimed_path = claimed.parent_path() / (claimed.filename().string() + ".claimed");
  std::filesystem::rename(claimed, claimed_path, ec);
  if (ec) {
    return false; // someone else claimed it first, or it vanished
  }

  const auto bytes = read_file(claimed_path);
  std::filesystem::remove(claimed_path, ec);

  const loaded_image image = backends_.loader->load(bytes.data(), bytes.size());

  std::size_t redirected = 0;
  for (const auto& replacement : image.replacements) {
    const auto* target = static_cast<const std::uint8_t*>(image.code) + replacement.offset_in_image;
    if (!backends_.substituter->patch_entry(replacement.old_entry,
                                            const_cast<std::uint8_t*>(target))) {
      throw std::runtime_error("failed to patch entry of " + replacement.name);
    }
    ++redirected;
  }

  neko::log(neko::log_level::ok, "reload applied: %zu function(s) redirected\n", redirected);
  return true;
}

} // namespace neko
