#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neko::detail {

struct group_descriptor;

/// One complete object record in an immutable managed generation. Paths are
/// portable lexical paths relative to the generation directory; this value
/// never opens or canonicalizes them.
struct generation_offer_member {
  std::string member;
  std::string object_path;
  std::string sha256;
  std::string source_identity;
  std::string build_information;

  bool operator==(const generation_offer_member&) const = default;
};

/// Platform-neutral contents of one managed generation manifest.
struct generation_offer {
  std::string group_id;
  std::uint64_t sequence = 0;
  std::string generation_id;
  std::string compatibility_id;
  std::string abi_id;
  std::vector<generation_offer_member> members;
  std::vector<std::string> changed_inputs;

  bool operator==(const generation_offer&) const = default;
};

/// Identity encoded in `<sequence>-<generation_id>.ready`.
struct generation_offer_reference {
  std::uint64_t sequence = 0;
  std::string generation_id;

  bool operator==(const generation_offer_reference&) const = default;
};

enum class generation_offer_error_code : std::uint8_t {
  invalid_format,
  unsupported_version,
  unknown_directive,
  malformed_value,
  unexpected_value,
  duplicate_field,
  missing_field,
  invalid_field,
  duplicate_member,
  duplicate_object_path,
  duplicate_changed_input,
  invalid_marker,
  marker_mismatch,
  group_mismatch,
  compatibility_mismatch,
  abi_mismatch,
  membership_mismatch,
};

class generation_offer_error final : public std::runtime_error {
public:
  generation_offer_error(generation_offer_error_code code, std::size_t line,
                         const std::string& message);

  [[nodiscard]] generation_offer_error_code code() const noexcept { return code_; }
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
  generation_offer_error_code code_;
  std::size_t line_;
};

inline constexpr std::string_view generation_offer_format_name = "nekomata-generation";
inline constexpr std::uint64_t generation_offer_format_version = 2;

[[nodiscard]]
std::string_view generation_offer_error_code_name(generation_offer_error_code code) noexcept;

/// Validates a manifest value without accessing the filesystem.
void validate_generation_offer(const generation_offer& offer, std::string_view source = "<memory>");

/// Parses and serializes the canonical line-oriented manifest payload.
[[nodiscard]] generation_offer parse_generation_offer(std::string_view text,
                                                      std::string_view source = "<memory>");
[[nodiscard]] std::string serialize_generation_offer(const generation_offer& offer);

/// Validates the manifest identity and ordered membership against the
/// descriptor linked into the running application.
void validate_generation_offer_against_descriptor(const generation_offer& offer,
                                                  const group_descriptor& descriptor,
                                                  std::string_view source = "<memory>");

/// Parses and serializes the immutable ready-marker filename. Marker contents
/// are empty; the filename identifies the generation directory and manifest.
[[nodiscard]] generation_offer_reference
parse_generation_offer_marker(std::string_view filename, std::string_view source = "<marker>");
[[nodiscard]] std::string
serialize_generation_offer_marker(const generation_offer_reference& reference);

/// Confirms that a selected ready marker names the manifest that was read.
void validate_generation_offer_reference(const generation_offer_reference& reference,
                                         const generation_offer& offer,
                                         std::string_view source = "<memory>");

} // namespace neko::detail
