// The shared transaction engine both watch surfaces drive: load and precheck
// objects without writing, link one generation, then commit with full
// rollback. No file system, no threads — the callers own discovery.

#include "session_impl.hpp"

#include <neko/log.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neko {

std::unique_ptr<reload_session::impl::prepared_reload>
reload_session::impl::prepare_object(const std::vector<std::uint8_t>& bytes, std::string watch_key,
                                     const std::filesystem::path& source_path,
                                     std::string build_information) {
  auto prepared = std::make_unique<prepared_reload>();
  prepared->watch_key = std::move(watch_key);
  prepared->build_information = std::move(build_information);
  prepared->image =
      backends_.loader->load(bytes.data(), bytes.size(), source_path.generic_string());
  if (prepared->image.allocation == nullptr) {
    throw std::runtime_error("object loader returned no executable allocation");
  }

  // Complete every zero-write check during preparation. A rejected object
  // therefore cannot reach the commit path or alter a live function entry.
  auto& image = prepared->image;
  for (const auto& replacement : image.replacements) {
    auto* target = static_cast<std::uint8_t*>(image.code()) + replacement.offset_in_image;
    if (!backends_.substituter->precheck_entry(replacement.old_entry, target)) {
      throw std::runtime_error("reload rejected before any write: cannot patch entry of " +
                               replacement.name);
    }
  }

  return prepared;
}

void reload_session::impl::link_generation(prepared_generation& generation) {
  std::vector<backend::loaded_image*> images;
  images.reserve(generation.reloads.size());
  for (const auto& reload : generation.reloads) {
    images.push_back(&reload->image);
  }
  backends_.loader->link_generation(images);
}

void reload_session::impl::validate_generation(const prepared_generation& generation) const {
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

void reload_session::impl::commit(prepared_generation& generation) {
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
  // Allocate bookkeeping capacity before the first live write. Moving the
  // successfully installed handles below is then noexcept, so a later
  // allocation failure cannot destroy code that entries already target.
  active_allocations_.reserve(active_allocations_.size() + generation.reloads.size());

  // Capture every rollback image before the first live write.
  for (auto& prepared : generation.reloads) {
    auto& image = prepared->image;
    for (const auto& replacement : image.replacements) {
      saved_entry entry{};
      entry.entry = replacement.old_entry;
      entry.target = static_cast<std::uint8_t*>(image.code()) + replacement.offset_in_image;
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

  // Entries now point into these images. Transfer each allocation from the
  // candidate transaction into the session before any later bookkeeping can
  // throw; rejected transactions never reach this ownership boundary.
  for (auto& prepared : generation.reloads) {
    active_allocations_.push_back(std::move(prepared->image.allocation));
  }

  // Warn about functions an updated object dropped. An object not present in
  // this transaction keeps its prior redirects and must not look stale.
  for (const auto& prepared : generation.reloads) {
    auto& last_redirected = last_redirected_by_object_[prepared->watch_key];
    for (const auto& previous : last_redirected) {
      const auto& name = previous.first;
      const bool still_present =
          std::any_of(prepared->image.replacements.begin(), prepared->image.replacements.end(),
                      [&](const backend::function_replacement& replacement) {
                        return replacement.name == name;
                      });
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
