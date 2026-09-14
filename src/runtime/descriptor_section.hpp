#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "group_descriptor.hpp"

namespace neko::detail {

/// One framed `neko-group-v1` payload and where it sat inside the section.
struct descriptor_record {
  group_descriptor descriptor;
  std::size_t offset = 0;
};

/// Parses the `neko_groups` input-section framing: a contiguous sequence of
/// records, each a four-byte little-endian payload length followed by exactly
/// that many payload bytes. Payloads are parsed as canonical
/// `neko-group-v1` text. A truncated or zero-length record throws
/// `std::runtime_error`; payload failures throw `descriptor_error` with the
/// record offset in the source name.
[[nodiscard]] std::vector<descriptor_record>
parse_descriptor_section(std::span<const std::uint8_t> bytes,
                         std::string_view source = "<section>");

/// Produces the same framing deterministically, for the build integration
/// and tests.
[[nodiscard]] std::vector<std::uint8_t>
serialize_descriptor_section(const std::vector<group_descriptor>& descriptors);

/// Collapses identical duplicate records and rejects conflicting ones: two
/// records sharing a `group_id` with different contents are a configuration
/// error. Order follows the first occurrence of each group.
[[nodiscard]] std::vector<group_descriptor>
resolve_descriptor_records(const std::vector<descriptor_record>& records);

} // namespace neko::detail
