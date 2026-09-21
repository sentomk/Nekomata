#pragma once

// Shared scanning leaves for the line-oriented wire formats: the group
// descriptor and the generation offer dialects read quoted fields, portable
// keys, and unsigned decimals the same way. These helpers own structure
// only; error codes and message texts stay with each codec, whose rejection
// wording is a tested contract.

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace neko::detail {

[[nodiscard]] inline bool contains_control_character(std::string_view value) {
  for (const unsigned char byte : value) {
    if (byte < 0x20 || byte == 0x7f) {
      return true;
    }
  }
  return false;
}

/// The magic line grammar shared by every wire format: a format name and its
/// version as two space-separated tokens. Codecs keep their own name and
/// version constants and own the classification of a rejected line.
[[nodiscard]] inline std::string compose_magic_line(std::string_view name, std::uint64_t version) {
  return std::string(name) + ' ' + std::to_string(version);
}

/// Structural findings for a magic line, in detection order. A line naming
/// the family but carrying another version — including the retired suffix
/// grammar `name-vN` — reports `wrong_version`, so codecs can distinguish an
/// unsupported version from a foreign format.
enum class magic_line_issue : std::uint8_t {
  none,
  wrong_name,
  wrong_version,
};

[[nodiscard]] inline magic_line_issue
inspect_magic_line(std::string_view line, std::string_view name, std::uint64_t version) {
  if (line == compose_magic_line(name, version)) {
    return magic_line_issue::none;
  }
  if (!line.starts_with(name)) {
    return magic_line_issue::wrong_name;
  }
  const std::string_view rest = line.substr(name.size());
  if (rest.empty() || rest.front() == ' ' || rest.front() == '-') {
    return magic_line_issue::wrong_version;
  }
  return magic_line_issue::wrong_name;
}

[[nodiscard]] inline bool is_portable_key_character(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.' || byte == '/';
}

/// Structural findings for a portable key, in detection order. `value` must
/// be non-empty (callers run their identity checks first).
enum class portable_key_issue : std::uint8_t {
  none,
  boundary_slash,
  non_portable_character,
  forbidden_component,
  invalid_component,
};

[[nodiscard]] inline portable_key_issue inspect_portable_key(std::string_view value,
                                                             bool allow_components) {
  if (value.front() == '/' || value.back() == '/') {
    return portable_key_issue::boundary_slash;
  }
  std::size_t component_begin = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size() && value[index] != '/') {
      if (!is_portable_key_character(static_cast<unsigned char>(value[index]))) {
        return portable_key_issue::non_portable_character;
      }
      continue;
    }
    if (!allow_components && index != value.size()) {
      return portable_key_issue::forbidden_component;
    }
    const auto component = value.substr(component_begin, index - component_begin);
    if (component.empty() || component == "." || component == "..") {
      return portable_key_issue::invalid_component;
    }
    component_begin = index + 1;
  }
  return portable_key_issue::none;
}

/// One double-quoted field read with `std::quoted` semantics.
struct quoted_field {
  std::string value;
  bool ok = false;         // field read successfully
  bool was_quoted = false; // the next token started with a quote
};

[[nodiscard]] inline quoted_field read_quoted_field(std::istringstream& row) {
  row >> std::ws;
  quoted_field field;
  field.was_quoted = row.peek() == '"';
  if (!field.was_quoted) {
    return field;
  }
  row >> std::quoted(field.value);
  field.ok = static_cast<bool>(row);
  return field;
}

/// True when the row still holds non-whitespace content after the fields
/// the caller consumed.
[[nodiscard]] inline bool has_trailing_fields(std::istringstream& row) {
  row >> std::ws;
  return !row.eof();
}

/// Unsigned decimal parse of one token. `ok` requires digits only and full
/// consumption; `leading_zero` flags non-canonical "0…" forms ("0" itself
/// is canonical).
struct unsigned_parse {
  std::uint64_t value = 0;
  bool ok = false;
  bool leading_zero = false;
};

[[nodiscard]] inline unsigned_parse parse_unsigned_decimal(std::string_view token) {
  unsigned_parse out;
  out.leading_zero = token.size() > 1 && token.front() == '0';
  const auto* begin = token.data();
  const auto* end = begin + token.size();
  const auto [next, error] = std::from_chars(begin, end, out.value);
  out.ok = !token.empty() && error == std::errc{} && next == end;
  return out;
}

} // namespace neko::detail
