#pragma once

#include "c/neko_sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace neko::detail {

/// SHA-256 (FIPS 180-4) over one contiguous buffer.
[[nodiscard]] inline std::array<std::uint8_t, 32>
sha256_digest(std::span<const std::uint8_t> data) {
  neko_sha256_context context;
  neko_sha256_init(&context);
  neko_sha256_update(&context, data.data(), data.size());
  std::array<std::uint8_t, 32> digest{};
  neko_sha256_final(&context, digest.data());
  return digest;
}

/// Lowercase hexadecimal SHA-256 of one contiguous buffer.
[[nodiscard]] inline std::string sha256_hex(std::span<const std::uint8_t> data) {
  static constexpr char k_digits[] = "0123456789abcdef";
  const auto digest = sha256_digest(data);
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    hex.push_back(k_digits[byte >> 4]);
    hex.push_back(k_digits[byte & 0x0f]);
  }
  return hex;
}

/// Lowercase hexadecimal SHA-256 of a string's bytes.
[[nodiscard]] inline std::string sha256_hex(std::string_view text) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
  return sha256_hex(std::span<const std::uint8_t>{bytes, text.size()});
}

} // namespace neko::detail
