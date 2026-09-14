#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "generation_offer.hpp"
#include "group_descriptor.hpp"

namespace neko::detail {

/// Outcome of one observation pass over a managed generation stream.
enum class stream_status : std::uint8_t {
  idle,     ///< no offer newer than the local cursor exists
  consumed, ///< a complete generation passed every check
  rejected, ///< the newest observed offer failed a check; `message` says why
};

struct generation_stream_observation {
  stream_status status = stream_status::idle;
  std::uint64_t sequence = 0; ///< observed offer, when consumed or rejected
  std::string generation_id;  ///< observed offer, when consumed or rejected
  std::string message;        ///< rejection reason; empty otherwise

  generation_offer offer;                     ///< validated manifest when consumed
  std::filesystem::path generation_directory; ///< immutable directory when consumed
  /// Digest-verified object files in descriptor member order, when consumed.
  std::vector<std::filesystem::path> objects;
};

/// Reads the immutable, multi-consumer publication stream of one reload group
/// below a generation root:
///
/// ```text
/// <generation_root>/<publication_key>/
///   offers/<sequence>-<generation_id>.ready
///   generations/<generation_id>/{manifest,objects/...}
/// ```
///
/// The stream is only ever read. Markers, manifests, and objects are never
/// renamed, deleted, or modified, so any number of independent consumers can
/// observe the same offers concurrently.
///
/// Each instance keeps its own cursor. The cursor starts at the descriptor's
/// baseline sequence, advances past every observed offer whether that offer is
/// consumed or rejected, and therefore reports a malformed offer once instead
/// of on every pass. When several offers are newer than the cursor, only the
/// newest is observed: greatest sequence first, then the lexicographically
/// greatest generation ID as the deterministic tie-break.
class generation_stream {
public:
  generation_stream(group_descriptor descriptor, std::filesystem::path generation_root);

  [[nodiscard]] const group_descriptor& descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] const std::filesystem::path& generation_root() const noexcept { return root_; }
  [[nodiscard]] std::uint64_t cursor() const noexcept { return cursor_; }

  /// Performs one observation pass. Artifact problems are rejections (values),
  /// not exceptions; only programming errors throw.
  [[nodiscard]] generation_stream_observation poll();

private:
  [[nodiscard]] generation_stream_observation
  reject(std::uint64_t sequence, std::string generation_id, std::string message);

  group_descriptor descriptor_;
  std::filesystem::path root_;
  std::uint64_t cursor_ = 0;
  std::set<std::string> reported_bad_markers_;
};

} // namespace neko::detail
