#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neko::detail {

struct generation_object_offer {
  std::filesystem::path object_path;
  std::filesystem::path source_path;
  std::string build_information;
};

struct generation_offer {
  std::string id;
  std::vector<std::filesystem::path> changed_files;
  std::vector<generation_object_offer> objects;
};

/// Atomically claims and parses a published generation manifest. Relative
/// paths in the manifest are resolved against the manifest's directory.
std::optional<generation_offer>
try_claim_generation_manifest(const std::filesystem::path& manifest_path);

} // namespace neko::detail
