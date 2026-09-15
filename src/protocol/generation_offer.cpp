#include <protocol/generation_offer.hpp>

#include <protocol/group_descriptor.hpp>

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
  std::string message = "invalid generation offer '" + std::string(source) + "'";
  if (line != 0) {
    message += " at line " + std::to_string(line);
  }
  return message + ": " + std::string(reason);
}

[[noreturn]] void reject(generation_offer_error_code code, std::string_view source,
                         std::size_t line, std::string_view reason) {
  throw generation_offer_error{code, line, error_message(source, line, reason)};
}

void require_identity(
    std::string_view value, std::string_view field, std::string_view source, std::size_t line,
    generation_offer_error_code code = generation_offer_error_code::invalid_field) {
  if (value.empty()) {
    reject(code, source, line, std::string(field) + " must not be empty");
  }
  if (contains_control_character(value)) {
    reject(code, source, line, std::string(field) + " must not contain control characters");
  }
}

void require_text(std::string_view value, std::string_view field, std::string_view source,
                  std::size_t line) {
  if (contains_control_character(value)) {
    reject(generation_offer_error_code::invalid_field, source, line,
           std::string(field) + " must not contain control characters");
  }
}

void require_portable_path(
    std::string_view value, std::string_view field, bool allow_components, std::string_view source,
    std::size_t line,
    generation_offer_error_code code = generation_offer_error_code::invalid_field) {
  require_identity(value, field, source, line, code);
  switch (inspect_portable_key(value, allow_components)) {
  case portable_key_issue::none:
    break;
  case portable_key_issue::boundary_slash:
    reject(code, source, line, std::string(field) + " must be a relative portable path");
  case portable_key_issue::non_portable_character:
    reject(code, source, line, std::string(field) + " contains a non-portable character");
  case portable_key_issue::forbidden_component:
    reject(code, source, line, std::string(field) + " must be one path component");
  case portable_key_issue::invalid_component:
    reject(code, source, line, std::string(field) + " contains an invalid path component");
  }
}

void require_sha256(std::string_view value, std::string_view source, std::size_t line) {
  const bool valid = value.size() == 64 && std::ranges::all_of(value, [](unsigned char byte) {
                       return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
                     });
  if (!valid) {
    reject(generation_offer_error_code::invalid_field, source, line,
           "sha256 must contain exactly 64 lowercase hexadecimal digits");
  }
}

void require_end(std::istringstream& row, std::string_view source, std::size_t line) {
  if (has_trailing_fields(row)) {
    reject(generation_offer_error_code::unexpected_value, source, line,
           "unexpected trailing fields");
  }
}

[[nodiscard]] std::string read_quoted(std::istringstream& row, std::string_view source,
                                      std::size_t line, std::string_view field) {
  const auto scanned = read_quoted_field(row);
  if (!scanned.was_quoted) {
    reject(generation_offer_error_code::malformed_value, source, line,
           std::string(field) + " must be quoted");
  }
  if (!scanned.ok) {
    reject(generation_offer_error_code::malformed_value, source, line,
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
    reject(generation_offer_error_code::malformed_value, source, line,
           "sequence must be a canonical unsigned decimal integer");
  }
  return parsed.value;
}

void validate_member(const generation_offer_member& member, std::string_view source,
                     std::size_t line) {
  require_portable_path(member.member, "member", true, source, line);
  require_portable_path(member.object_path, "object_path", true, source, line);
  if (!member.object_path.starts_with("objects/") || member.object_path.size() == 8) {
    reject(generation_offer_error_code::invalid_field, source, line,
           "object_path must be below objects/");
  }
  require_sha256(member.sha256, source, line);
  require_identity(member.source_identity, "source_identity", source, line);
  require_text(member.build_information, "build_information", source, line);
}

void validate_reference(const generation_offer_reference& reference, std::string_view source,
                        generation_offer_error_code code) {
  require_portable_path(reference.generation_id, "generation_id", false, source, 0, code);
}

} // namespace

generation_offer_error::generation_offer_error(generation_offer_error_code code, std::size_t line,
                                               const std::string& message)
    : std::runtime_error(message), code_(code), line_(line) {}

std::string_view generation_offer_error_code_name(generation_offer_error_code code) noexcept {
  switch (code) {
  case generation_offer_error_code::invalid_format:
    return "invalid_format";
  case generation_offer_error_code::unsupported_version:
    return "unsupported_version";
  case generation_offer_error_code::unknown_directive:
    return "unknown_directive";
  case generation_offer_error_code::malformed_value:
    return "malformed_value";
  case generation_offer_error_code::unexpected_value:
    return "unexpected_value";
  case generation_offer_error_code::duplicate_field:
    return "duplicate_field";
  case generation_offer_error_code::missing_field:
    return "missing_field";
  case generation_offer_error_code::invalid_field:
    return "invalid_field";
  case generation_offer_error_code::duplicate_member:
    return "duplicate_member";
  case generation_offer_error_code::duplicate_object_path:
    return "duplicate_object_path";
  case generation_offer_error_code::duplicate_changed_input:
    return "duplicate_changed_input";
  case generation_offer_error_code::invalid_marker:
    return "invalid_marker";
  case generation_offer_error_code::marker_mismatch:
    return "marker_mismatch";
  case generation_offer_error_code::group_mismatch:
    return "group_mismatch";
  case generation_offer_error_code::compatibility_mismatch:
    return "compatibility_mismatch";
  case generation_offer_error_code::abi_mismatch:
    return "abi_mismatch";
  case generation_offer_error_code::membership_mismatch:
    return "membership_mismatch";
  }
  return "unknown";
}

void validate_generation_offer(const generation_offer& offer, std::string_view source) {
  require_identity(offer.group_id, "group_id", source, 0);
  require_portable_path(offer.generation_id, "generation_id", false, source, 0);
  require_identity(offer.compatibility_id, "compatibility_id", source, 0);
  require_identity(offer.abi_id, "abi_id", source, 0);
  if (offer.members.empty()) {
    reject(generation_offer_error_code::missing_field, source, 0,
           "at least one member is required");
  }

  std::unordered_set<std::string> members;
  std::unordered_set<std::string> object_paths;
  for (const auto& member : offer.members) {
    validate_member(member, source, 0);
    if (!members.insert(member.member).second) {
      reject(generation_offer_error_code::duplicate_member, source, 0,
             "duplicate member '" + member.member + "'");
    }
    if (!object_paths.insert(member.object_path).second) {
      reject(generation_offer_error_code::duplicate_object_path, source, 0,
             "duplicate object_path '" + member.object_path + "'");
    }
  }

  std::unordered_set<std::string> changed_inputs;
  for (const auto& input : offer.changed_inputs) {
    require_identity(input, "changed_input", source, 0);
    if (!changed_inputs.insert(input).second) {
      reject(generation_offer_error_code::duplicate_changed_input, source, 0,
             "duplicate changed_input '" + input + "'");
    }
  }
}

generation_offer parse_generation_offer(std::string_view text, std::string_view source) {
  std::istringstream input{std::string(text)};
  std::string line;
  std::size_t line_number = 0;
  if (!std::getline(input, line)) {
    reject(generation_offer_error_code::invalid_format, source, 1, "missing format header");
  }
  ++line_number;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != generation_offer_format) {
    const auto code = line.starts_with("nekomata-generation-")
                          ? generation_offer_error_code::unsupported_version
                          : generation_offer_error_code::invalid_format;
    reject(code, source, line_number, "expected " + std::string(generation_offer_format));
  }

  generation_offer offer;
  bool has_group_id = false;
  bool has_sequence = false;
  bool has_generation_id = false;
  bool has_compatibility_id = false;
  bool has_abi_id = false;
  std::unordered_set<std::string> members;
  std::unordered_set<std::string> object_paths;
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
        reject(generation_offer_error_code::duplicate_field, source, line_number,
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
        reject(generation_offer_error_code::duplicate_field, source, line_number,
               "duplicate sequence");
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
    if (directive == "compatibility_id") {
      read_identity_field(has_compatibility_id, offer.compatibility_id, "compatibility_id");
      continue;
    }
    if (directive == "abi_id") {
      read_identity_field(has_abi_id, offer.abi_id, "abi_id");
      continue;
    }
    if (directive == "member") {
      generation_offer_member member;
      member.member = read_quoted(row, source, line_number, "member");
      member.object_path = read_quoted(row, source, line_number, "object_path");
      member.sha256 = read_quoted(row, source, line_number, "sha256");
      member.source_identity = read_quoted(row, source, line_number, "source_identity");
      member.build_information = read_quoted(row, source, line_number, "build_information");
      require_end(row, source, line_number);
      validate_member(member, source, line_number);
      if (!members.insert(member.member).second) {
        reject(generation_offer_error_code::duplicate_member, source, line_number,
               "duplicate member '" + member.member + "'");
      }
      if (!object_paths.insert(member.object_path).second) {
        reject(generation_offer_error_code::duplicate_object_path, source, line_number,
               "duplicate object_path '" + member.object_path + "'");
      }
      offer.members.push_back(std::move(member));
      continue;
    }
    if (directive == "changed_input") {
      std::string changed_input = read_quoted(row, source, line_number, "changed_input");
      require_end(row, source, line_number);
      require_identity(changed_input, "changed_input", source, line_number);
      if (!changed_inputs.insert(changed_input).second) {
        reject(generation_offer_error_code::duplicate_changed_input, source, line_number,
               "duplicate changed_input '" + changed_input + "'");
      }
      offer.changed_inputs.push_back(std::move(changed_input));
      continue;
    }

    reject(generation_offer_error_code::unknown_directive, source, line_number,
           "unknown directive '" + directive + "'");
  }

  const auto require_field = [&](bool present, std::string_view field) {
    if (!present) {
      reject(generation_offer_error_code::missing_field, source, line_number + 1,
             "missing " + std::string(field));
    }
  };
  require_field(has_group_id, "group_id");
  require_field(has_sequence, "sequence");
  require_field(has_generation_id, "generation_id");
  require_field(has_compatibility_id, "compatibility_id");
  require_field(has_abi_id, "abi_id");
  if (offer.members.empty()) {
    reject(generation_offer_error_code::missing_field, source, line_number + 1,
           "at least one member is required");
  }

  validate_generation_offer(offer, source);
  return offer;
}

std::string serialize_generation_offer(const generation_offer& offer) {
  validate_generation_offer(offer);

  std::ostringstream output;
  output << generation_offer_format << '\n';
  output << "group_id " << std::quoted(offer.group_id) << '\n';
  output << "sequence " << offer.sequence << '\n';
  output << "generation_id " << std::quoted(offer.generation_id) << '\n';
  output << "compatibility_id " << std::quoted(offer.compatibility_id) << '\n';
  output << "abi_id " << std::quoted(offer.abi_id) << '\n';
  for (const auto& member : offer.members) {
    output << "member " << std::quoted(member.member) << ' ' << std::quoted(member.object_path)
           << ' ' << std::quoted(member.sha256) << ' ' << std::quoted(member.source_identity) << ' '
           << std::quoted(member.build_information) << '\n';
  }
  for (const auto& input : offer.changed_inputs) {
    output << "changed_input " << std::quoted(input) << '\n';
  }
  return output.str();
}

void validate_generation_offer_against_descriptor(const generation_offer& offer,
                                                  const group_descriptor& descriptor,
                                                  std::string_view source) {
  validate_generation_offer(offer, source);
  validate_group_descriptor(descriptor);

  if (offer.group_id != descriptor.group_id) {
    reject(generation_offer_error_code::group_mismatch, source, 0,
           "group_id '" + offer.group_id + "' does not match descriptor '" + descriptor.group_id +
               "'");
  }
  if (offer.compatibility_id != descriptor.compatibility_id) {
    reject(generation_offer_error_code::compatibility_mismatch, source, 0,
           "compatibility_id does not match descriptor");
  }
  if (offer.abi_id != descriptor.abi_id) {
    reject(generation_offer_error_code::abi_mismatch, source, 0,
           "abi_id does not match descriptor");
  }
  if (offer.members.size() != descriptor.members.size()) {
    reject(generation_offer_error_code::membership_mismatch, source, 0,
           "member count " + std::to_string(offer.members.size()) +
               " does not match descriptor count " + std::to_string(descriptor.members.size()));
  }
  for (std::size_t index = 0; index < offer.members.size(); ++index) {
    if (offer.members[index].member != descriptor.members[index]) {
      reject(generation_offer_error_code::membership_mismatch, source, 0,
             "member at index " + std::to_string(index) + " is '" + offer.members[index].member +
                 "', expected '" + descriptor.members[index] + "'");
    }
  }
}

generation_offer_reference parse_generation_offer_marker(std::string_view filename,
                                                         std::string_view source) {
  constexpr std::string_view suffix = ".ready";
  if (!filename.ends_with(suffix)) {
    reject(generation_offer_error_code::invalid_marker, source, 0, "marker must end in .ready");
  }
  const auto stem = filename.substr(0, filename.size() - suffix.size());
  const auto separator = stem.find('-');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 == stem.size()) {
    reject(generation_offer_error_code::invalid_marker, source, 0,
           "marker must be <sequence>-<generation_id>.ready");
  }

  generation_offer_reference reference;
  const auto parsed = parse_unsigned_decimal(stem.substr(0, separator));
  if (parsed.leading_zero || !parsed.ok) {
    reject(generation_offer_error_code::invalid_marker, source, 0,
           "sequence must be a canonical unsigned decimal integer");
  }
  reference.sequence = parsed.value;
  reference.generation_id = std::string(stem.substr(separator + 1));
  validate_reference(reference, source, generation_offer_error_code::invalid_marker);
  return reference;
}

std::string serialize_generation_offer_marker(const generation_offer_reference& reference) {
  validate_reference(reference, "<memory>", generation_offer_error_code::invalid_marker);
  return std::to_string(reference.sequence) + "-" + reference.generation_id + ".ready";
}

void validate_generation_offer_reference(const generation_offer_reference& reference,
                                         const generation_offer& offer, std::string_view source) {
  validate_reference(reference, source, generation_offer_error_code::invalid_marker);
  validate_generation_offer(offer, source);
  if (reference.sequence != offer.sequence) {
    reject(generation_offer_error_code::marker_mismatch, source, 0,
           "marker sequence " + std::to_string(reference.sequence) +
               " does not match manifest sequence " + std::to_string(offer.sequence));
  }
  if (reference.generation_id != offer.generation_id) {
    reject(generation_offer_error_code::marker_mismatch, source, 0,
           "marker generation_id '" + reference.generation_id +
               "' does not match manifest generation_id '" + offer.generation_id + "'");
  }
}

} // namespace neko::detail
