#include "group_descriptor.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace neko::detail {
namespace {

[[nodiscard]] std::string error_message(std::string_view source, std::size_t line,
                                        std::string_view reason) {
  std::string message = "invalid reload group descriptor '" + std::string(source) + "'";
  if (line != 0) {
    message += " at line " + std::to_string(line);
  }
  return message + ": " + std::string(reason);
}

[[noreturn]] void reject(descriptor_error_code code, std::string_view source, std::size_t line,
                         std::string_view reason) {
  throw descriptor_error{code, line, error_message(source, line, reason)};
}

[[nodiscard]] bool contains_control_character(std::string_view value) {
  for (const unsigned char byte : value) {
    if (byte < 0x20 || byte == 0x7f) {
      return true;
    }
  }
  return false;
}

void require_identity(std::string_view value, std::string_view field, std::string_view source,
                      std::size_t line) {
  if (value.empty()) {
    reject(descriptor_error_code::invalid_field, source, line,
           std::string(field) + " must not be empty");
  }
  if (contains_control_character(value)) {
    reject(descriptor_error_code::invalid_field, source, line,
           std::string(field) + " must not contain control characters");
  }
}

[[nodiscard]] bool is_portable_key_character(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.' || byte == '/';
}

void require_portable_key(std::string_view value, std::string_view field, bool allow_components,
                          std::string_view source, std::size_t line) {
  require_identity(value, field, source, line);
  if (value.front() == '/' || value.back() == '/') {
    reject(descriptor_error_code::invalid_field, source, line,
           std::string(field) + " must be a relative logical key");
  }

  std::size_t component_begin = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size() && value[index] != '/') {
      if (!is_portable_key_character(static_cast<unsigned char>(value[index]))) {
        reject(descriptor_error_code::invalid_field, source, line,
               std::string(field) + " contains a non-portable character");
      }
      continue;
    }

    if (!allow_components && index != value.size()) {
      reject(descriptor_error_code::invalid_field, source, line,
             std::string(field) + " must be one path component");
    }
    const auto component = value.substr(component_begin, index - component_begin);
    if (component.empty() || component == "." || component == "..") {
      reject(descriptor_error_code::invalid_field, source, line,
             std::string(field) + " contains an invalid path component");
    }
    component_begin = index + 1;
  }
}

void require_end(std::istringstream& row, std::string_view source, std::size_t line) {
  row >> std::ws;
  if (!row.eof()) {
    reject(descriptor_error_code::unexpected_value, source, line, "unexpected trailing fields");
  }
}

[[nodiscard]] std::string read_quoted(std::istringstream& row, std::string_view source,
                                      std::size_t line, std::string_view field) {
  row >> std::ws;
  if (row.peek() != '"') {
    reject(descriptor_error_code::malformed_value, source, line,
           std::string(field) + " must be quoted");
  }
  std::string value;
  row >> std::quoted(value);
  if (!row) {
    reject(descriptor_error_code::malformed_value, source, line,
           "cannot read " + std::string(field));
  }
  return value;
}

[[nodiscard]] std::uint64_t read_sequence(std::istringstream& row, std::string_view source,
                                          std::size_t line) {
  std::string value;
  row >> value;
  if (value.empty()) {
    reject(descriptor_error_code::malformed_value, source, line, "cannot read baseline_sequence");
  }
  std::uint64_t sequence = 0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto [next, error] = std::from_chars(begin, end, sequence);
  if (error != std::errc{} || next != end) {
    reject(descriptor_error_code::malformed_value, source, line,
           "baseline_sequence must be an unsigned decimal integer");
  }
  return sequence;
}

} // namespace

descriptor_error::descriptor_error(descriptor_error_code code, std::size_t line,
                                   const std::string& message)
    : std::runtime_error(message), code_(code), line_(line) {}

std::string_view descriptor_error_code_name(descriptor_error_code code) noexcept {
  switch (code) {
  case descriptor_error_code::invalid_format:
    return "invalid_format";
  case descriptor_error_code::unsupported_version:
    return "unsupported_version";
  case descriptor_error_code::unknown_directive:
    return "unknown_directive";
  case descriptor_error_code::malformed_value:
    return "malformed_value";
  case descriptor_error_code::unexpected_value:
    return "unexpected_value";
  case descriptor_error_code::duplicate_field:
    return "duplicate_field";
  case descriptor_error_code::missing_field:
    return "missing_field";
  case descriptor_error_code::invalid_field:
    return "invalid_field";
  case descriptor_error_code::duplicate_member:
    return "duplicate_member";
  }
  return "unknown";
}

void validate_group_descriptor(const group_descriptor& descriptor, std::string_view source) {
  require_identity(descriptor.group_id, "group_id", source, 0);
  require_portable_key(descriptor.publication_key, "publication_key", false, source, 0);
  require_identity(descriptor.compatibility_id, "compatibility_id", source, 0);
  require_identity(descriptor.abi_id, "abi_id", source, 0);
  if (descriptor.generation_root_hint) {
    require_identity(*descriptor.generation_root_hint, "generation_root_hint", source, 0);
  }
  if (descriptor.members.empty()) {
    reject(descriptor_error_code::missing_field, source, 0, "at least one member is required");
  }

  std::unordered_set<std::string> members;
  for (const auto& member : descriptor.members) {
    require_portable_key(member, "member", true, source, 0);
    if (!members.insert(member).second) {
      reject(descriptor_error_code::duplicate_member, source, 0,
             "duplicate member '" + member + "'");
    }
  }
}

group_descriptor parse_group_descriptor(std::string_view text, std::string_view source) {
  std::istringstream input{std::string(text)};
  std::string line;
  std::size_t line_number = 0;
  if (!std::getline(input, line)) {
    reject(descriptor_error_code::invalid_format, source, 1, "missing format header");
  }
  ++line_number;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != group_descriptor_format) {
    const descriptor_error_code code = line.starts_with("nekomata-group-")
                                           ? descriptor_error_code::unsupported_version
                                           : descriptor_error_code::invalid_format;
    reject(code, source, line_number, "expected " + std::string(group_descriptor_format));
  }

  group_descriptor descriptor;
  bool has_group_id = false;
  bool has_publication_key = false;
  bool has_baseline_sequence = false;
  bool has_compatibility_id = false;
  bool has_abi_id = false;
  bool has_generation_root_hint = false;
  std::unordered_set<std::string> members;

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
        reject(descriptor_error_code::duplicate_field, source, line_number,
               "duplicate " + std::string(field));
      }
      destination = read_quoted(row, source, line_number, field);
      require_end(row, source, line_number);
      require_identity(destination, field, source, line_number);
      present = true;
    };

    if (directive == "group_id") {
      read_identity_field(has_group_id, descriptor.group_id, "group_id");
      continue;
    }
    if (directive == "publication_key") {
      read_identity_field(has_publication_key, descriptor.publication_key, "publication_key");
      require_portable_key(descriptor.publication_key, "publication_key", false, source,
                           line_number);
      continue;
    }
    if (directive == "baseline_sequence") {
      if (has_baseline_sequence) {
        reject(descriptor_error_code::duplicate_field, source, line_number,
               "duplicate baseline_sequence");
      }
      descriptor.baseline_sequence = read_sequence(row, source, line_number);
      require_end(row, source, line_number);
      has_baseline_sequence = true;
      continue;
    }
    if (directive == "compatibility_id") {
      read_identity_field(has_compatibility_id, descriptor.compatibility_id, "compatibility_id");
      continue;
    }
    if (directive == "abi_id") {
      read_identity_field(has_abi_id, descriptor.abi_id, "abi_id");
      continue;
    }
    if (directive == "generation_root_hint") {
      if (has_generation_root_hint) {
        reject(descriptor_error_code::duplicate_field, source, line_number,
               "duplicate generation_root_hint");
      }
      descriptor.generation_root_hint =
          read_quoted(row, source, line_number, "generation_root_hint");
      require_end(row, source, line_number);
      require_identity(*descriptor.generation_root_hint, "generation_root_hint", source,
                       line_number);
      has_generation_root_hint = true;
      continue;
    }
    if (directive == "member") {
      std::string member = read_quoted(row, source, line_number, "member");
      require_end(row, source, line_number);
      require_portable_key(member, "member", true, source, line_number);
      if (!members.insert(member).second) {
        reject(descriptor_error_code::duplicate_member, source, line_number,
               "duplicate member '" + member + "'");
      }
      descriptor.members.push_back(std::move(member));
      continue;
    }

    reject(descriptor_error_code::unknown_directive, source, line_number,
           "unknown directive '" + directive + "'");
  }

  const auto require_field = [&](bool present, std::string_view field) {
    if (!present) {
      reject(descriptor_error_code::missing_field, source, line_number + 1,
             "missing " + std::string(field));
    }
  };
  require_field(has_group_id, "group_id");
  require_field(has_publication_key, "publication_key");
  require_field(has_baseline_sequence, "baseline_sequence");
  require_field(has_compatibility_id, "compatibility_id");
  require_field(has_abi_id, "abi_id");
  if (descriptor.members.empty()) {
    reject(descriptor_error_code::missing_field, source, line_number + 1,
           "at least one member is required");
  }

  validate_group_descriptor(descriptor, source);
  return descriptor;
}

std::string serialize_group_descriptor(const group_descriptor& descriptor) {
  validate_group_descriptor(descriptor);

  std::ostringstream output;
  output << group_descriptor_format << '\n';
  output << "group_id " << std::quoted(descriptor.group_id) << '\n';
  output << "publication_key " << std::quoted(descriptor.publication_key) << '\n';
  output << "baseline_sequence " << descriptor.baseline_sequence << '\n';
  output << "compatibility_id " << std::quoted(descriptor.compatibility_id) << '\n';
  output << "abi_id " << std::quoted(descriptor.abi_id) << '\n';
  if (descriptor.generation_root_hint) {
    output << "generation_root_hint " << std::quoted(*descriptor.generation_root_hint) << '\n';
  }
  for (const auto& member : descriptor.members) {
    output << "member " << std::quoted(member) << '\n';
  }
  return output.str();
}

} // namespace neko::detail
