// Legacy watch claiming and preparation: atomically claim a ready object or
// manifest, plan its translation units, and hand the loaded images to the
// shared transaction engine.

#include "session_impl.hpp"

#include "legacy/generation.hpp"

#include <base/file.hpp>
#include <neko/log.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

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

  const std::filesystem::path& operator[](std::size_t index) const { return paths_[index]; }

private:
  std::vector<std::filesystem::path> paths_;
};

std::size_t validate_generation_membership(const detail::legacy_generation_offer& offer,
                                           const backend::patch_planner& planner) {
  backend::change_set changes;
  changes.changed_files.reserve(offer.changed_files.size());
  for (const auto& changed : offer.changed_files) {
    changes.changed_files.push_back(changed.generic_string());
  }

  const auto plans = planner.plan(changes);
  std::vector<std::string> expected_order;
  std::unordered_set<std::string> expected;
  for (const auto& plan : plans) {
    for (const auto& translation_unit : plan.translation_units) {
      const auto key = detail::normalized_source_path(translation_unit).generic_string();
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

std::unique_ptr<reload_session::impl::prepared_generation>
reload_session::impl::try_prepare(const generation_watch& watched) {
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
    std::error_code ready_ec;
    if (!std::filesystem::is_regular_file(object.object_path, ready_ec)) {
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
    const auto bytes = detail::read_required_bytes(object.object_path, "cannot open object file: ");
    generation->reloads.push_back(prepare_object(bytes, object.source_path.generic_string(),
                                                 object.source_path, object.build_information));
  }
  link_generation(*generation);
  return generation;
}

} // namespace neko
