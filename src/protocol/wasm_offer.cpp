#include <protocol/wasm_offer.hpp>

#include <protocol/scanner.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace neko::detail {
namespace {

[[nodiscard]] std::string error_message(std::string_view source, std::size_t line,
                                        std::string_view reason) {
  std::string message = "invalid wasm offer '" + std::string(source) + "'";
  if (line != 0) {
    message += " at line " + std::to_string(line);
  }
  return message + ": " + std::string(reason);
}

[[noreturn]] void reject(wasm_offer_error_code code, std::string_view source, std::size_t line,
                         std::string_view reason) {
  throw wasm_offer_error{code, line, error_message(source, line, reason)};
}

void require_identity(std::string_view value, std::string_view field, std::string_view source,
                      std::size_t line,
                      wasm_offer_error_code code = wasm_offer_error_code::invalid_field) {
  if (value.empty()) {
    reject(code, source, line, std::string(field) + " must not be empty");
  }
  if (contains_control_character(value)) {
    reject(code, source, line, std::string(field) + " must not contain control characters");
  }
}

void require_portable_path(std::string_view value, std::string_view field, bool allow_components,
                           std::string_view source, std::size_t line) {
  require_identity(value, field, source, line);
  switch (inspect_portable_key(value, allow_components)) {
  case portable_key_issue::none:
    break;
  case portable_key_issue::boundary_slash:
    reject(wasm_offer_error_code::invalid_field, source, line,
           std::string(field) + " must be a relative portable path");
  case portable_key_issue::non_portable_character:
    reject(wasm_offer_error_code::invalid_field, source, line,
           std::string(field) + " contains a non-portable character");
  case portable_key_issue::forbidden_component:
    reject(wasm_offer_error_code::invalid_field, source, line,
           std::string(field) + " must be one path component");
  case portable_key_issue::invalid_component:
    reject(wasm_offer_error_code::invalid_field, source, line,
           std::string(field) + " contains an invalid path component");
  }
}

void require_sha256(std::string_view value, std::string_view source, std::size_t line) {
  const bool valid = value.size() == 64 && std::ranges::all_of(value, [](unsigned char byte) {
                       return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
                     });
  if (!valid) {
    reject(wasm_offer_error_code::invalid_field, source, line,
           "sha256 must contain exactly 64 lowercase hexadecimal digits");
  }
}

void require_end(std::istringstream& row, std::string_view source, std::size_t line) {
  if (has_trailing_fields(row)) {
    reject(wasm_offer_error_code::unexpected_value, source, line, "unexpected trailing fields");
  }
}

[[nodiscard]] std::string read_quoted(std::istringstream& row, std::string_view source,
                                      std::size_t line, std::string_view field) {
  auto scanned = read_quoted_field(row);
  if (!scanned.was_quoted) {
    reject(wasm_offer_error_code::malformed_value, source, line,
           std::string(field) + " must be quoted");
  }
  if (!scanned.ok) {
    reject(wasm_offer_error_code::malformed_value, source, line,
           "cannot read " + std::string(field));
  }
  return std::move(scanned.value);
}

[[nodiscard]] std::uint64_t read_sequence(std::istringstream& row, std::string_view source,
                                          std::size_t line) {
  std::string value;
  row >> value;
  const auto parsed = parse_unsigned_decimal(value);
  if (value.empty() || parsed.leading_zero || !parsed.ok) {
    reject(wasm_offer_error_code::malformed_value, source, line,
           "sequence must be a canonical unsigned decimal integer");
  }
  return parsed.value;
}

} // namespace

wasm_offer_error::wasm_offer_error(wasm_offer_error_code code, std::size_t line,
                                   const std::string& message)
    : std::runtime_error(message), code_(code), line_(line) {}

std::string_view wasm_offer_error_code_name(wasm_offer_error_code code) noexcept {
  switch (code) {
  case wasm_offer_error_code::invalid_format:
    return "invalid_format";
  case wasm_offer_error_code::unsupported_version:
    return "unsupported_version";
  case wasm_offer_error_code::unknown_directive:
    return "unknown_directive";
  case wasm_offer_error_code::malformed_value:
    return "malformed_value";
  case wasm_offer_error_code::unexpected_value:
    return "unexpected_value";
  case wasm_offer_error_code::duplicate_field:
    return "duplicate_field";
  case wasm_offer_error_code::missing_field:
    return "missing_field";
  case wasm_offer_error_code::invalid_field:
    return "invalid_field";
  case wasm_offer_error_code::duplicate_entry:
    return "duplicate_entry";
  case wasm_offer_error_code::duplicate_changed_input:
    return "duplicate_changed_input";
  }
  return "unknown";
}

void validate_wasm_offer(const wasm_offer& offer, std::string_view source) {
  require_identity(offer.group_id, "group_id", source, 0);
  require_portable_path(offer.generation_id, "generation_id", false, source, 0);
  require_identity(offer.abi_id, "abi_id", source, 0);
  require_portable_path(offer.artifact_path, "artifact_path", true, source, 0);
  if (!offer.artifact_path.starts_with("modules/") || offer.artifact_path.size() == 8) {
    reject(wasm_offer_error_code::invalid_field, source, 0, "artifact_path must be below modules/");
  }
  require_sha256(offer.sha256, source, 0);

  if (offer.entries.empty()) {
    reject(wasm_offer_error_code::missing_field, source, 0, "at least one entry is required");
  }
  std::unordered_set<std::string> entries;
  for (const auto& entry : offer.entries) {
    require_identity(entry, "entry", source, 0);
    if (!entries.insert(entry).second) {
      reject(wasm_offer_error_code::duplicate_entry, source, 0, "duplicate entry '" + entry + "'");
    }
  }

  std::unordered_set<std::string> changed_inputs;
  for (const auto& input : offer.changed_inputs) {
    require_identity(input, "changed_input", source, 0);
    if (!changed_inputs.insert(input).second) {
      reject(wasm_offer_error_code::duplicate_changed_input, source, 0,
             "duplicate changed_input '" + input + "'");
    }
  }
}

wasm_offer parse_wasm_offer(std::string_view text, std::string_view source) {
  std::istringstream input{std::string(text)};
  std::string line;
  std::size_t line_number = 0;
  if (!std::getline(input, line)) {
    reject(wasm_offer_error_code::invalid_format, source, 1, "missing format header");
  }
  ++line_number;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != wasm_offer_format) {
    const auto code = line.starts_with("nekomata-wasm-")
                          ? wasm_offer_error_code::unsupported_version
                          : wasm_offer_error_code::invalid_format;
    reject(code, source, line_number, "expected " + std::string(wasm_offer_format));
  }

  wasm_offer offer;
  bool has_group_id = false;
  bool has_sequence = false;
  bool has_generation_id = false;
  bool has_abi_id = false;
  bool has_artifact = false;
  std::unordered_set<std::string> entries;
  std::unordered_set<std::string> changed_inputs;

  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::istringstream row{line};
    row >> std::ws;
    if (row.eof() || row.peek() == '#') {
      continue;
    }

    std::string directive;
    row >> directive;
    const auto read_identity_field = [&](bool& present, std::string& destination,
                                         std::string_view field) {
      if (present) {
        reject(wasm_offer_error_code::duplicate_field, source, line_number,
               "duplicate " + std::string(field));
      }
      destination = read_quoted(row, source, line_number, field);
      require_end(row, source, line_number);
      require_identity(destination, field, source, line_number);
      present = true;
    };

    if (directive == "group_id") {
      read_identity_field(has_group_id, offer.group_id, "group_id");
      continue;
    }
    if (directive == "sequence") {
      if (has_sequence) {
        reject(wasm_offer_error_code::duplicate_field, source, line_number, "duplicate sequence");
      }
      offer.sequence = read_sequence(row, source, line_number);
      require_end(row, source, line_number);
      has_sequence = true;
      continue;
    }
    if (directive == "generation_id") {
      read_identity_field(has_generation_id, offer.generation_id, "generation_id");
      require_portable_path(offer.generation_id, "generation_id", false, source, line_number);
      continue;
    }
    if (directive == "abi_id") {
      read_identity_field(has_abi_id, offer.abi_id, "abi_id");
      continue;
    }
    if (directive == "artifact") {
      if (has_artifact) {
        reject(wasm_offer_error_code::duplicate_field, source, line_number, "duplicate artifact");
      }
      offer.artifact_path = read_quoted(row, source, line_number, "artifact_path");
      offer.sha256 = read_quoted(row, source, line_number, "sha256");
      require_end(row, source, line_number);
      require_portable_path(offer.artifact_path, "artifact_path", true, source, line_number);
      if (!offer.artifact_path.starts_with("modules/") || offer.artifact_path.size() == 8) {
        reject(wasm_offer_error_code::invalid_field, source, line_number,
               "artifact_path must be below modules/");
      }
      require_sha256(offer.sha256, source, line_number);
      has_artifact = true;
      continue;
    }
    if (directive == "entry") {
      std::string entry = read_quoted(row, source, line_number, "entry");
      require_end(row, source, line_number);
      require_identity(entry, "entry", source, line_number);
      if (!entries.insert(entry).second) {
        reject(wasm_offer_error_code::duplicate_entry, source, line_number,
               "duplicate entry '" + entry + "'");
      }
      offer.entries.push_back(std::move(entry));
      continue;
    }
    if (directive == "changed_input") {
      std::string changed_input = read_quoted(row, source, line_number, "changed_input");
      require_end(row, source, line_number);
      require_identity(changed_input, "changed_input", source, line_number);
      if (!changed_inputs.insert(changed_input).second) {
        reject(wasm_offer_error_code::duplicate_changed_input, source, line_number,
               "duplicate changed_input '" + changed_input + "'");
      }
      offer.changed_inputs.push_back(std::move(changed_input));
      continue;
    }

    reject(wasm_offer_error_code::unknown_directive, source, line_number,
           "unknown directive '" + directive + "'");
  }

  const auto require_field = [&](bool present, std::string_view field) {
    if (!present) {
      reject(wasm_offer_error_code::missing_field, source, line_number + 1,
             "missing " + std::string(field));
    }
  };
  require_field(has_group_id, "group_id");
  require_field(has_sequence, "sequence");
  require_field(has_generation_id, "generation_id");
  require_field(has_abi_id, "abi_id");
  require_field(has_artifact, "artifact");
  if (offer.entries.empty()) {
    reject(wasm_offer_error_code::missing_field, source, line_number + 1,
           "at least one entry is required");
  }

  validate_wasm_offer(offer, source);
  return offer;
}

std::string serialize_wasm_offer(const wasm_offer& offer) {
  validate_wasm_offer(offer);

  std::ostringstream output;
  output << wasm_offer_format << '\n';
  output << "group_id " << std::quoted(offer.group_id) << '\n';
  output << "sequence " << offer.sequence << '\n';
  output << "generation_id " << std::quoted(offer.generation_id) << '\n';
  output << "abi_id " << std::quoted(offer.abi_id) << '\n';
  output << "artifact " << std::quoted(offer.artifact_path) << ' ' << std::quoted(offer.sha256)
         << '\n';
  for (const auto& entry : offer.entries) {
    output << "entry " << std::quoted(entry) << '\n';
  }
  for (const auto& input : offer.changed_inputs) {
    output << "changed_input " << std::quoted(input) << '\n';
  }
  return output.str();
}

} // namespace neko::detail
