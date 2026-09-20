#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neko::detail {

/// One browser reload generation: a single immutable WASM artifact, its
/// digest, and the ordered entry set the application will activate. Paths
/// are portable lexical paths relative to the generation directory; this
/// value never fetches or opens them.
struct wasm_offer {
  std::string group_id;
  std::uint64_t sequence = 0;
  std::string generation_id;
  /// Declared identity covering entry signatures and persistent state
  /// layout; checked against the module descriptor before activation.
  std::string abi_id;
  std::string artifact_path;
  std::string sha256;
  std::vector<std::string> entries;
  std::vector<std::string> changed_inputs;

  bool operator==(const wasm_offer&) const = default;
};

enum class wasm_offer_error_code : std::uint8_t {
  invalid_format,
  unsupported_version,
  unknown_directive,
  malformed_value,
  unexpected_value,
  duplicate_field,
  missing_field,
  invalid_field,
  duplicate_entry,
  duplicate_changed_input,
};

class wasm_offer_error final : public std::runtime_error {
public:
  wasm_offer_error(wasm_offer_error_code code, std::size_t line, const std::string& message);

  [[nodiscard]] wasm_offer_error_code code() const noexcept { return code_; }
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
  wasm_offer_error_code code_;
  std::size_t line_;
};

inline constexpr std::string_view wasm_offer_format = "nekomata-wasm-v1";

[[nodiscard]] std::string_view wasm_offer_error_code_name(wasm_offer_error_code code) noexcept;

/// Validates a manifest value without accessing the network or filesystem.
void validate_wasm_offer(const wasm_offer& offer, std::string_view source = "<memory>");

/// Parses and serializes the canonical line-oriented manifest payload.
[[nodiscard]] wasm_offer parse_wasm_offer(std::string_view text,
                                          std::string_view source = "<memory>");
[[nodiscard]] std::string serialize_wasm_offer(const wasm_offer& offer);

} // namespace neko::detail
