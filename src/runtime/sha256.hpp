#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace neko::detail {

/// SHA-256 (FIPS 180-4) over one contiguous buffer.
[[nodiscard]] std::array<std::uint8_t, 32> sha256_digest(std::span<const std::uint8_t> data);

/// Lowercase hexadecimal SHA-256 of one contiguous buffer.
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> data);

/// Lowercase hexadecimal SHA-256 of a string's bytes.
[[nodiscard]] std::string sha256_hex(std::string_view text);

} // namespace neko::detail
