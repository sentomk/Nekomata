#include "generation_stream.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <tuple>
#include <utility>

#include "sha256.hpp"

namespace neko::detail {
namespace {

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (ec || !std::filesystem::is_regular_file(path, ec)) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return std::nullopt;
  }
  return content;
}

bool is_beneath(const std::filesystem::path& child, const std::filesystem::path& ancestor) {
  const auto relative_text = child.lexically_relative(ancestor).generic_string();
  return !relative_text.empty() && relative_text != "." && relative_text != ".." &&
         relative_text.rfind("../", 0) != 0;
}

} // namespace

generation_stream::generation_stream(group_descriptor descriptor,
                                     const std::filesystem::path& generation_root)
    : descriptor_(std::move(descriptor)), root_(std::filesystem::absolute(generation_root)),
      cursor_(descriptor_.baseline_sequence) {
  validate_group_descriptor(descriptor_);
}

generation_stream_observation generation_stream::poll() {
  const auto publication = root_ / descriptor_.publication_key;
  const auto offers = publication / "offers";

  std::error_code ec;
  if (!std::filesystem::is_directory(offers, ec) || ec) {
    return {}; // absent root or stream stays idle; a later pass may retry
  }

  std::vector<generation_offer_reference> newer;
  std::vector<std::string> malformed;
  try {
    for (const auto& entry : std::filesystem::directory_iterator(offers)) {
      const auto name = entry.path().filename().string();
      if (!name.ends_with(".ready")) {
        continue;
      }
      try {
        const auto reference = parse_generation_offer_marker(name, name);
        if (reference.sequence > cursor_) {
          newer.push_back(reference);
        }
      } catch (const generation_offer_error&) {
        malformed.push_back(name);
      }
    }
  } catch (const std::filesystem::filesystem_error&) {
    return {}; // unreadable directory now is not an offer observation
  }

  if (newer.empty()) {
    // A malformed marker carries no sequence, so it cannot advance the
    // cursor. Report it once, then stay quiet until a valid offer appears.
    std::sort(malformed.begin(), malformed.end());
    for (const auto& name : malformed) {
      if (reported_bad_markers_.insert(name).second) {
        generation_stream_observation observation;
        observation.status = stream_status::rejected;
        observation.message = "generation stream '" + descriptor_.publication_key +
                              "': offer marker '" + name + "' is not a valid offer marker";
        return observation;
      }
    }
    return {};
  }

  // Newest wins: greatest sequence, then lexicographically greatest
  // generation ID as the deterministic tie-break.
  const auto selected =
      *std::max_element(newer.begin(), newer.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sequence, a.generation_id) < std::tie(b.sequence, b.generation_id);
      });

  const auto generation_directory = publication / "generations" / selected.generation_id;
  const auto manifest_path = generation_directory / "manifest";

  const auto manifest_text = read_file(manifest_path);
  if (!manifest_text) {
    return reject(selected.sequence, selected.generation_id,
                  "generation stream '" + descriptor_.publication_key + "': generation '" +
                      selected.generation_id + "' has no readable manifest at '" +
                      manifest_path.generic_string() + "'");
  }

  try {
    auto offer = parse_generation_offer(*manifest_text, manifest_path.generic_string());
    validate_generation_offer(offer, manifest_path.generic_string());
    validate_generation_offer_against_descriptor(offer, descriptor_,
                                                 manifest_path.generic_string());
    validate_generation_offer_reference(selected, offer,
                                        serialize_generation_offer_marker(selected));

    const auto canonical_directory = std::filesystem::canonical(generation_directory);
    std::vector<std::filesystem::path> objects;
    for (const auto& member : offer.members) {
      const auto joined = generation_directory / std::filesystem::path(member.object_path);
      const auto resolved = std::filesystem::weakly_canonical(joined, ec);
      if (ec || !is_beneath(resolved, canonical_directory)) {
        return reject(selected.sequence, selected.generation_id,
                      "generation stream '" + descriptor_.publication_key + "': member '" +
                          member.member + "' object '" + member.object_path +
                          "' resolves outside the generation directory");
      }
      const auto bytes = read_file(resolved);
      if (!bytes) {
        return reject(selected.sequence, selected.generation_id,
                      "generation stream '" + descriptor_.publication_key + "': member '" +
                          member.member + "' object '" + member.object_path +
                          "' is missing or not a regular file");
      }
      const auto computed = sha256_hex(*bytes);
      if (computed != member.sha256) {
        return reject(selected.sequence, selected.generation_id,
                      "generation stream '" + descriptor_.publication_key + "': member '" +
                          member.member + "' object '" + member.object_path +
                          "' digest mismatch: expected '" + member.sha256 + "', computed '" +
                          computed + "'");
      }
      objects.push_back(resolved);
    }

    generation_stream_observation observation;
    observation.status = stream_status::consumed;
    observation.sequence = selected.sequence;
    observation.generation_id = selected.generation_id;
    observation.offer = std::move(offer);
    observation.generation_directory = generation_directory;
    observation.objects = std::move(objects);
    cursor_ = selected.sequence;
    return observation;
  } catch (const generation_offer_error& error) {
    return reject(selected.sequence, selected.generation_id, error.what());
  }
}

generation_stream_observation
generation_stream::reject(std::uint64_t sequence, std::string generation_id, std::string message) {
  cursor_ = sequence; // an observed offer is past the cursor for good
  generation_stream_observation observation;
  observation.status = stream_status::rejected;
  observation.sequence = sequence;
  observation.generation_id = std::move(generation_id);
  observation.message = std::move(message);
  return observation;
}

} // namespace neko::detail
