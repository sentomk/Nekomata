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

std::filesystem::path normalized_source_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
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

struct reload_session::prepared_reload {
  loaded_image image;
};

reload_session::reload_session(backend_bundle backends) : backends_(std::move(backends)) {
  if (!backends_.loader || !backends_.symbols || !backends_.state || !backends_.substituter) {
    throw std::runtime_error("reload_session: incomplete backend bundle");
  }
  if (!backends_.planner) {
    backends_.planner = std::make_unique<trivial_planner>();
  }
}

reload_session::stats reload_session::session_stats() const {
  stats out;
  out.applied = applied_;
  out.rejected = rejected_;
  out.last_result = last_result_;
  for (const auto& watched : watched_) {
    out.watched_paths.push_back(watched.object_path.string());
  }
  return out;
}

void reload_session::watch(std::filesystem::path object_path) {
  watched_.push_back({std::move(object_path), {}});
}

void reload_session::watch(std::filesystem::path object_path,
                           const std::filesystem::path& source_path) {
  watched_.push_back({std::move(object_path), normalized_source_path(source_path)});
}

bool reload_session::update() {
  bool reloaded = false;
  for (const auto& watched : watched_) {
    try {
      if (auto prepared = try_prepare(watched)) {
        commit(*prepared);
        reloaded = true;
      }
    } catch (const std::exception& e) {
      ++rejected_;
      last_result_ = e.what();
      throw; // the caller decides how to surface the failure
    }
  }
  return reloaded;
}

std::unique_ptr<reload_session::prepared_reload>
reload_session::try_prepare(const watched_object& watched) {
  const auto& path = watched.object_path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return nullptr;
  }

  // Claim the file atomically before touching it, so a writer mid-flight
  // cannot hand us a partial object. The rename is the "explicit confirm"
  // of the reload trigger model.
  const auto& claimed = path;
  const auto claimed_path = claimed.parent_path() / (claimed.filename().string() + ".claimed");
  std::filesystem::rename(claimed, claimed_path, ec);
  if (ec) {
    return nullptr; // someone else claimed it first, or it vanished
  }

  const auto bytes = read_file(claimed_path);
  std::filesystem::remove(claimed_path, ec);

  const auto source_path = watched.source_path.generic_string();
  auto prepared = std::make_unique<prepared_reload>();
  prepared->image = backends_.loader->load(bytes.data(), bytes.size(), source_path);

  // Plan first (the planner owns "what does this object cover"): it is the
  // seam the future dependency graph grows into.
  change_set changes;
  changes.changed_files.push_back(source_path.empty() ? path.string() : source_path);
  const auto plans = backends_.planner->plan(changes);
  neko::log(neko::log_level::info, "plan covers %zu translation unit(s)\n", plans.size());

  // Complete every zero-write check during preparation. A rejected object
  // therefore cannot reach the commit path or alter a live function entry.
  const auto& image = prepared->image;
  for (const auto& replacement : image.replacements) {
    auto* target = static_cast<std::uint8_t*>(image.code) + replacement.offset_in_image;
    if (!backends_.substituter->precheck_entry(replacement.old_entry, target)) {
      throw std::runtime_error("reload rejected before any write: cannot patch entry of " +
                               replacement.name);
    }
  }

  return prepared;
}

void reload_session::commit(const prepared_reload& prepared) {
  const auto& image = prepared.image;

  // Snapshot and patch only after the complete object passed preparation.
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
  ++applied_;
  last_result_ = "applied " + std::to_string(image.replacements.size()) + " function(s)";
}

} // namespace neko
