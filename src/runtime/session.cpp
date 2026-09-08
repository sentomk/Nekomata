#include <neko/session.hpp>

#include <neko/log.hpp>
#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/patch_planner.hpp>

#include <algorithm>
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

// Trivial planner: every changed file is one translation unit to rebuild.
// Real dependency graphs (compiler .d files) are planned.
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

  // Plan first (the planner owns "what does this object cover"): it is the
  // seam the future dependency graph grows into.
  change_set changes;
  changes.changed_files.push_back(path.string());
  const auto plans = backends_.planner->plan(changes);
  neko::log(neko::log_level::info, "plan covers %zu translation unit(s)\n", plans.size());

  // Two-phase commit: precheck EVERY entry without writing a byte, snapshot
  // every entry, then patch. Any failure rolls already-written entries back
  // in reverse order, so a reload is all-or-nothing.
  for (const auto& replacement : image.replacements) {
    auto* target = static_cast<std::uint8_t*>(image.code) + replacement.offset_in_image;
    if (!backends_.substituter->precheck_entry(replacement.old_entry, target)) {
      throw std::runtime_error("reload rejected before any write: cannot patch entry of " +
                               replacement.name);
    }
  }
  struct saved_entry {
    std::uintptr_t entry;
    std::uint8_t original[5];
  };
  std::vector<saved_entry> saved;
  saved.reserve(image.replacements.size());
  for (const auto& replacement : image.replacements) {
    auto* target = static_cast<std::uint8_t*>(image.code) + replacement.offset_in_image;
    saved_entry entry{};
    entry.entry = replacement.old_entry;
    if (!backends_.substituter->snapshot_entry(replacement.old_entry, entry.original) ||
        !backends_.substituter->patch_entry(replacement.old_entry, target)) {
      for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        backends_.substituter->restore_entry(it->entry, it->original);
      }
      throw std::runtime_error("reload rejected and rolled back " + std::to_string(saved.size()) +
                               " entr" + (saved.size() == 1 ? "y" : "ies") +
                               ": cannot patch entry of " + replacement.name);
    }
    saved.push_back(entry);
  }

  // Warn about functions this load dropped: their old entries still jump to
  // the previous arena copy, so calls silently run stale code.
  for (const auto& previous : last_redirected_) {
    const auto& name = previous.first;
    const bool still_present =
        std::any_of(image.replacements.begin(), image.replacements.end(),
                    [&](const function_replacement& repl) { return repl.name == name; });
    if (!still_present) {
      neko::log(neko::log_level::warn,
                "stale redirect: '%s' was removed but its entry still jumps to old code\n",
                name.c_str());
    }
  }
  last_redirected_.clear();
  for (const auto& replacement : image.replacements) {
    last_redirected_[replacement.name] = replacement.old_entry;
  }

  neko::log(neko::log_level::ok, "reload applied: %zu function(s) redirected\n",
            image.replacements.size());
  return true;
}

} // namespace neko
