#pragma once

#include <cstdint>
#include <vector>

#include "group_descriptor.hpp"

namespace neko::detail {

/// Bounds of the embedded `neko_groups` input section, when the linker
/// provided one. Returns false wherever the section is absent; the symbols
/// are weak, so this links and reports nothing on platforms or links without
/// an embedded section.
[[nodiscard]] bool embedded_descriptor_section(const std::uint8_t*& begin,
                                               const std::uint8_t*& end);

/// Reads, parses, and resolves every descriptor embedded in the running
/// program. Identical duplicates collapse; conflicting contents throw
/// `std::runtime_error`; a corrupted payload throws `descriptor_error`.
[[nodiscard]] std::vector<group_descriptor> discover_embedded_descriptors();

} // namespace neko::detail
