#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neko::detail {

/// One object to publish for a member of the reload group.
struct publish_member {
  std::string member;                ///< logical member key
  std::filesystem::path object_file; ///< source object on this host
  std::string source_identity;       ///< logical translation-unit identity
  std::string build_information;     ///< diagnostic build information
};

/// A complete generation to publish into one stream.
struct publish_request {
  std::filesystem::path generation_root;
  std::string publication_key;
  std::string group_id;
  std::string compatibility_id;
  std::string abi_id;
  std::vector<publish_member> members;
  std::vector<std::string> changed_inputs;
};

struct publish_result {
  std::uint64_t sequence = 0;
  std::string generation_id;
};

/// One browser side module to publish behind one polled offer URL.
struct wasm_publish_request {
  std::filesystem::path offer_root; ///< served directory holding `latest` and `modules/`
  std::string publication_key;      ///< publisher-side stream identity
  std::string group_id;
  std::string abi_id; ///< app-declared entry signatures and state layout
  std::filesystem::path module_file;
  std::vector<std::string> entries; ///< ordered entry membership the offer names
};

/// Publishes one complete, immutable generation under
/// `<generation_root>/<publication_key>/` and releases its ready offer
/// atomically. Concurrent publishers on one stream serialize under a stream
/// lock; the sequence is strictly increasing and gaps after failed attempts
/// are valid. Objects are copied into the generation directory, so the
/// source objects may change afterwards without corrupting the stream.
/// Deterministic content: republishing identical objects reuses the same
/// generation ID at the next sequence.
///
/// Throws `std::runtime_error` for unusable inputs; filesystem failures
/// leave only staging data behind, never a ready offer.
[[nodiscard]] publish_result publish_generation(const publish_request& request);

/// Publishes one `nekomata-wasm/1` offer under `<offer_root>`: the artifact
/// staged at an immutable sequence-named path below `modules/`, and the
/// manifest atomically replacing `latest` last — a crash leaves an
/// unreferenced artifact, never a torn manifest. Same discipline as the
/// managed stream: one lock serializes concurrent publishers, the sequence
/// is strictly increasing, and the generation identity is deterministic in
/// the ABI, module bytes, and entry membership. Throws `std::runtime_error`
/// for unusable inputs.
[[nodiscard]] publish_result publish_wasm_offer(const wasm_publish_request& request);

} // namespace neko::detail
