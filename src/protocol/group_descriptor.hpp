#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neko::detail {

/// Logical description of one managed reload group. Build adapters serialize
/// this value into the application; discovery and section framing are kept
/// separate so this model has no platform-specific representation.
struct group_descriptor {
  std::string group_id;
  std::vector<std::string> members;
  std::string publication_key;
  std::uint64_t baseline_sequence = 0;
  std::string compatibility_id;
  std::string abi_id;
  std::optional<std::string> generation_root_hint;

  bool operator==(const group_descriptor&) const = default;
};

enum class descriptor_error_code : std::uint8_t {
  invalid_format,
  unsupported_version,
  unknown_directive,
  malformed_value,
  unexpected_value,
  duplicate_field,
  missing_field,
  invalid_field,
  duplicate_member,
};

class descriptor_error final : public std::runtime_error {
public:
  descriptor_error(descriptor_error_code code, std::size_t line, const std::string& message);

  [[nodiscard]] descriptor_error_code code() const noexcept { return code_; }
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
  descriptor_error_code code_;
  std::size_t line_;
};

inline constexpr std::string_view group_descriptor_format_name = "nekomata-group";
inline constexpr std::uint64_t group_descriptor_format_version = 1;

[[nodiscard]] std::string_view descriptor_error_code_name(descriptor_error_code code) noexcept;

/// Validates a descriptor built in memory. Identity strings remain opaque;
/// validation never opens or canonicalizes a filesystem path.
void validate_group_descriptor(const group_descriptor& descriptor,
                               std::string_view source = "<memory>");

/// Parses the canonical descriptor payload without section framing.
[[nodiscard]] group_descriptor parse_group_descriptor(std::string_view text,
                                                      std::string_view source = "<memory>");

/// Produces a deterministic payload after validating the value.
[[nodiscard]] std::string serialize_group_descriptor(const group_descriptor& descriptor);

} // namespace neko::detail
