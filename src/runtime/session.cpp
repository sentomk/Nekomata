#include <neko/session.hpp>

#include "descriptor_discovery.hpp"
#include "generation.hpp"
#include "generation_stream.hpp"

#include <neko/log.hpp>
#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/patch_planner.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
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

  const std::filesystem::path& operator[](std::size_t index) const { return paths_[index]; }

private:
  std::vector<std::filesystem::path> paths_;
};

std::filesystem::path claimed_path_for(const std::filesystem::path& path) {
  return path.parent_path() / (path.filename().string() + ".claimed");
}

std::size_t validate_generation_membership(const detail::legacy_generation_offer& offer,
                                           const patch_planner& planner) {
  change_set changes;
  changes.changed_files.reserve(offer.changed_files.size());
  for (const auto& changed : offer.changed_files) {
    changes.changed_files.push_back(changed.generic_string());
  }

  const auto plans = planner.plan(changes);
  std::vector<std::string> expected_order;
  std::unordered_set<std::string> expected;
  for (const auto& plan : plans) {
    for (const auto& translation_unit : plan.translation_units) {
      const auto key = normalized_source_path(translation_unit).generic_string();
      if (expected.insert(key).second) {
        expected_order.push_back(key);
      }
    }
  }
  if (expected.empty()) {
    throw std::runtime_error("reload generation '" + offer.id +
                             "' rejected before any write: planner returned no translation units");
  }

  std::unordered_set<std::string> actual;
  for (const auto& object : offer.objects) {
    actual.insert(object.source_path.generic_string());
  }
  for (const auto& expected_source : expected_order) {
    if (!actual.contains(expected_source)) {
      throw std::runtime_error("reload generation '" + offer.id +
                               "' rejected before any write: missing planned source '" +
                               expected_source + "'");
    }
  }
  for (const auto& object : offer.objects) {
    const auto source = object.source_path.generic_string();
    if (!expected.contains(source)) {
      throw std::runtime_error("reload generation '" + offer.id +
                               "' rejected before any write: unplanned source '" + source + "'");
    }
  }
  return expected.size();
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
  std::string watch_key;
  std::string build_information;
  loaded_image image;
};

struct reload_session::prepared_generation {
  std::string id;
  std::string manifest_key;
  std::vector<std::unique_ptr<prepared_reload>> reloads;
};

struct reload_session::managed_group {
  detail::group_descriptor descriptor;
  std::unique_ptr<detail::generation_stream> stream;
  bool enabled = false;
};

reload_session::reload_session(backend_bundle backends) : backends_(std::move(backends)) {
  if (!backends_.loader || !backends_.symbols || !backends_.state || !backends_.substituter) {
    throw std::runtime_error("reload_session: incomplete backend bundle");
  }
  if (!backends_.planner) {
    backends_.planner = std::make_unique<trivial_planner>();
  }

  // Descriptor discovery is a construction-time configuration step: corrupt
  // or conflicting embedded descriptors are configuration exceptions. The
  // groups stay disabled until watch() names them.
  for (auto& descriptor : detail::discover_embedded_descriptors()) {
    auto group = std::make_unique<managed_group>();
    group->descriptor = std::move(descriptor);
    managed_groups_.push_back(std::move(group));
  }
  std::sort(managed_groups_.begin(), managed_groups_.end(),
            [](const std::unique_ptr<managed_group>& a, const std::unique_ptr<managed_group>& b) {
              return a->descriptor.group_id < b->descriptor.group_id;
            });
}

reload_session::~reload_session() = default;

reload_session::stats reload_session::session_stats() const {
  stats out;
  out.applied = applied_;
  out.rejected = rejected_;
  out.last_result = last_result_;
  for (const auto& watched : watched_) {
    out.watched_paths.push_back(watched.object_path.string());
  }
  for (const auto& watched : generation_watches_) {
    out.watched_paths.push_back(watched.manifest_path.string());
  }
  for (const auto& group : managed_groups_) {
    if (group->enabled) {
      out.watched_paths.push_back(group->descriptor.group_id);
    }
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

void reload_session::watch(generation_watch generation) {
  generation.manifest_path = normalized_source_path(generation.manifest_path);
  generation_watches_.push_back(std::move(generation));
}

void reload_session::watch() {
  if (managed_groups_.empty()) {
    throw std::runtime_error(
        "reload_session: no embedded reload group descriptors; nothing to watch");
  }
  for (auto& group : managed_groups_) {
    enable_managed_group(*group);
  }
}

void reload_session::watch(std::string_view group_id) {
  enable_managed_group(find_managed_group(group_id));
}

void reload_session::watch(const char* group_id) {
  watch(std::string_view{group_id});
}

void reload_session::unwatch() {
  for (auto& group : managed_groups_) {
    group->enabled = false;
  }
}

void reload_session::unwatch(std::string_view group_id) {
  find_managed_group(group_id).enabled = false;
}

void reload_session::unwatch(const char* group_id) {
  unwatch(std::string_view{group_id});
}

void reload_session::enable_managed_group(managed_group& group) {
  if (group.enabled) {
    return;
  }
  if (!group.descriptor.generation_root_hint || group.descriptor.generation_root_hint->empty()) {
    throw std::runtime_error("reload_session: reload group '" + group.descriptor.group_id +
                             "' has no generation root hint");
  }
  // The stream outlives disable/enable cycles: its cursor is the group's
  // consumption state and must not replay already-observed generations.
  if (!group.stream) {
    group.stream = std::make_unique<detail::generation_stream>(
        group.descriptor, *group.descriptor.generation_root_hint);
  }
  group.enabled = true;
}

reload_session::managed_group& reload_session::find_managed_group(std::string_view group_id) {
  const auto found = std::find_if(managed_groups_.begin(), managed_groups_.end(),
                                  [group_id](const std::unique_ptr<managed_group>& group) {
                                    return group->descriptor.group_id == group_id;
                                  });
  if (found == managed_groups_.end()) {
    throw std::runtime_error("reload_session: unknown reload group '" + std::string{group_id} +
                             "'");
  }
  return **found;
}

bool reload_session::update() {
  try {
    // Managed groups come first, in group-ID order. One call commits at most
    // one generation overall, matching the single-transaction result model;
    // a ready group rejected here throws, and a not-yet-processed group's
    // offer is picked up by the next call.
    for (auto& group : managed_groups_) {
      if (!group->enabled) {
        continue;
      }
      const auto observation = group->stream->poll();
      if (observation.status == detail::stream_status::idle) {
        continue;
      }
      if (observation.status == detail::stream_status::rejected) {
        throw std::runtime_error(observation.message);
      }

      neko::log(neko::log_level::info, "generation '%s' covers %zu translation unit(s)\n",
                observation.generation_id.c_str(), observation.offer.members.size());
      prepared_generation generation;
      generation.id = observation.generation_id;
      generation.manifest_key = "managed:" + group->descriptor.group_id;
      generation.reloads.reserve(observation.objects.size());
      for (std::size_t index = 0; index < observation.objects.size(); ++index) {
        const auto& member = observation.offer.members[index];
        generation.reloads.push_back(prepare_object(
            read_file(observation.objects[index]), group->descriptor.group_id + "/" + member.member,
            std::filesystem::path(member.source_identity), member.build_information));
      }
      validate_generation(generation);
      commit(generation);
      return true;
    }

    prepared_generation generation;
    for (const auto& watched : generation_watches_) {
      if (auto offered_generation = try_prepare(watched)) {
        validate_generation(*offered_generation);
        commit(*offered_generation);
        return true;
      }
    }

    for (const auto& watched : watched_) {
      if (auto prepared = try_prepare(watched)) {
        generation.reloads.push_back(std::move(prepared));
      }
    }
    if (generation.reloads.empty()) {
      return false;
    }

    validate_generation(generation);
    commit(generation);
    return true;
  } catch (const std::exception& e) {
    ++rejected_;
    last_result_ = e.what();
    throw; // the caller decides how to surface the failure
  }
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
  const auto claimed_path = claimed_path_for(path);
  std::filesystem::rename(path, claimed_path, ec);
  if (ec) {
    return nullptr; // someone else claimed it first, or it vanished
  }
  claimed_files cleanup;
  cleanup.add(claimed_path);

  const auto bytes = read_file(claimed_path);

  const auto source_path = watched.source_path.generic_string();

  // Plan first (the planner owns "what does this object cover"): it is the
  // seam the future dependency graph grows into.
  change_set changes;
  changes.changed_files.push_back(source_path.empty() ? path.string() : source_path);
  const auto plans = backends_.planner->plan(changes);
  neko::log(neko::log_level::info, "plan covers %zu translation unit(s)\n", plans.size());
  return prepare_object(bytes, path.lexically_normal().generic_string(), watched.source_path, {});
}

std::unique_ptr<reload_session::prepared_generation>
reload_session::try_prepare(const generation_watch& watched) {
  auto offered = detail::try_claim_generation_manifest(watched.manifest_path);
  if (!offered) {
    return nullptr;
  }
  auto offer = std::move(*offered);

  const auto manifest_key = watched.manifest_path.generic_string();
  const auto applied = applied_generation_ids_by_manifest_.find(manifest_key);
  if (applied != applied_generation_ids_by_manifest_.end() && applied->second.contains(offer.id)) {
    throw std::runtime_error("reload generation '" + offer.id + "' was already applied");
  }

  const auto planned_count = validate_generation_membership(offer, *backends_.planner);
  neko::log(neko::log_level::info, "generation '%s' covers %zu translation unit(s)\n",
            offer.id.c_str(), planned_count);

  for (const auto& object : offer.objects) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(object.object_path, ec)) {
      throw std::runtime_error(
          "reload generation '" + offer.id +
          "' rejected before any write: object file is not ready: " + object.object_path.string());
    }
  }

  auto generation = std::make_unique<prepared_generation>();
  generation->id = offer.id;
  generation->manifest_key = manifest_key;
  generation->reloads.reserve(offer.objects.size());
  for (const auto& object : offer.objects) {
    const auto bytes = read_file(object.object_path);
    generation->reloads.push_back(prepare_object(bytes, object.source_path.generic_string(),
                                                 object.source_path, object.build_information));
  }
  return generation;
}

std::unique_ptr<reload_session::prepared_reload>
reload_session::prepare_object(const std::vector<std::uint8_t>& bytes, std::string watch_key,
                               const std::filesystem::path& source_path,
                               std::string build_information) {
  auto prepared = std::make_unique<prepared_reload>();
  prepared->watch_key = std::move(watch_key);
  prepared->build_information = std::move(build_information);
  prepared->image =
      backends_.loader->load(bytes.data(), bytes.size(), source_path.generic_string());

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

void reload_session::validate_generation(const prepared_generation& generation) const {
  std::unordered_map<std::uintptr_t, std::string> entries;
  for (const auto& prepared : generation.reloads) {
    for (const auto& replacement : prepared->image.replacements) {
      if (!entries.emplace(replacement.old_entry, replacement.name).second) {
        throw std::runtime_error("reload rejected before any write: multiple objects replace entry "
                                 "of " +
                                 replacement.name);
      }
    }
  }
}

void reload_session::commit(const prepared_generation& generation) {
  // Snapshot and patch only after the complete ready-object batch passed
  // preparation. The saved list spans every object, so rollback does too.
  struct saved_entry {
    std::uintptr_t entry;
    void* target;
    const std::string* name;
    std::uint8_t original[5];
  };

  std::size_t replacement_count = 0;
  for (const auto& prepared : generation.reloads) {
    replacement_count += prepared->image.replacements.size();
  }

  std::vector<saved_entry> saved;
  saved.reserve(replacement_count);

  // Capture every rollback image before the first live write.
  for (const auto& prepared : generation.reloads) {
    const auto& image = prepared->image;
    for (const auto& replacement : image.replacements) {
      saved_entry entry{};
      entry.entry = replacement.old_entry;
      entry.target = static_cast<std::uint8_t*>(image.code) + replacement.offset_in_image;
      entry.name = &replacement.name;
      if (!backends_.substituter->snapshot_entry(entry.entry, entry.original)) {
        throw std::runtime_error(
            "reload rejected and rolled back 0 entries: cannot patch entry of " + *entry.name);
      }
      saved.push_back(entry);
    }
  }

  std::size_t patched = 0;
  for (const auto& entry : saved) {
    if (!backends_.substituter->patch_entry(entry.entry, entry.target)) {
      for (std::size_t index = patched; index > 0; --index) {
        const auto& written = saved[index - 1];
        backends_.substituter->restore_entry(written.entry, written.original);
      }
      throw std::runtime_error("reload rejected and rolled back " + std::to_string(patched) +
                               " entr" + (patched == 1 ? "y" : "ies") + ": cannot patch entry of " +
                               *entry.name);
    }
    ++patched;
  }

  // Warn about functions an updated object dropped. An object not present in
  // this transaction keeps its prior redirects and must not look stale.
  for (const auto& prepared : generation.reloads) {
    auto& last_redirected = last_redirected_by_object_[prepared->watch_key];
    for (const auto& previous : last_redirected) {
      const auto& name = previous.first;
      const bool still_present = std::any_of(
          prepared->image.replacements.begin(), prepared->image.replacements.end(),
          [&](const function_replacement& replacement) { return replacement.name == name; });
      if (!still_present) {
        neko::log(neko::log_level::warn,
                  "stale redirect: '%s' was removed but its entry still jumps to old code\n",
                  name.c_str());
      }
    }
    last_redirected.clear();
    for (const auto& replacement : prepared->image.replacements) {
      last_redirected[replacement.name] = replacement.old_entry;
    }
  }

  ++applied_;
  if (generation.id.empty()) {
    neko::log(neko::log_level::ok, "reload applied: %zu function(s) redirected\n",
              replacement_count);
    last_result_ = "applied " + std::to_string(replacement_count) + " function(s)";
    return;
  }

  applied_generation_ids_by_manifest_[generation.manifest_key].insert(generation.id);
  neko::log(neko::log_level::ok, "reload generation '%s' applied: %zu function(s) redirected\n",
            generation.id.c_str(), replacement_count);
  last_result_ = "applied generation '" + generation.id +
                 "': " + std::to_string(replacement_count) + " function(s)";
}

} // namespace neko
