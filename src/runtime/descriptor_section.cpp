#include "descriptor_section.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace neko::detail {
namespace {

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
  }
}

std::uint32_t read_u32_le(const std::uint8_t* bytes) {
  return std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8) |
         (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
}

std::string record_source(std::string_view source, std::size_t offset) {
  return std::string{source} + " record at offset " + std::to_string(offset);
}

} // namespace

std::vector<descriptor_record> parse_descriptor_section(std::span<const std::uint8_t> bytes,
                                                        std::string_view source) {
  const auto trailing_zeros = [&bytes](std::size_t from) {
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
  };

  std::vector<descriptor_record> records;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    // Linkers align sections; zero bytes after the final record are padding.
    if (bytes.size() - offset < sizeof(std::uint32_t)) {
      if (trailing_zeros(offset)) {
        break;
      }
      throw std::runtime_error("invalid descriptor section '" + std::string{source} +
                               "': record at offset " + std::to_string(offset) + " is truncated");
    }
    const auto length = read_u32_le(bytes.data() + offset);
    if (length == 0) {
      if (trailing_zeros(offset)) {
        break;
      }
      throw std::runtime_error("invalid descriptor section '" + std::string{source} +
                               "': record at offset " + std::to_string(offset) +
                               " has a zero length");
    }
    const std::size_t payload_offset = offset + sizeof(std::uint32_t);
    if (length > bytes.size() - payload_offset) {
      throw std::runtime_error("invalid descriptor section '" + std::string{source} +
                               "': record at offset " + std::to_string(offset) + " is truncated");
    }
    const auto* payload = reinterpret_cast<const char*>(bytes.data() + payload_offset);
    records.push_back(
        {parse_group_descriptor({payload, length}, record_source(source, offset)), offset});
    offset = payload_offset + length;
  }
  return records;
}

std::vector<std::uint8_t>
serialize_descriptor_section(const std::vector<group_descriptor>& descriptors) {
  std::vector<std::uint8_t> bytes;
  for (const auto& descriptor : descriptors) {
    const auto payload = serialize_group_descriptor(descriptor);
    append_u32_le(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }
  return bytes;
}

std::vector<group_descriptor>
resolve_descriptor_records(const std::vector<descriptor_record>& records) {
  std::vector<group_descriptor> resolved;
  std::vector<std::size_t> offsets;
  for (const auto& record : records) {
    const auto found = std::find_if(resolved.begin(), resolved.end(),
                                    [&record](const group_descriptor& descriptor) {
                                      return descriptor.group_id == record.descriptor.group_id;
                                    });
    if (found == resolved.end()) {
      resolved.push_back(record.descriptor);
      offsets.push_back(record.offset);
      continue;
    }
    if (*found != record.descriptor) {
      const auto conflict = std::distance(resolved.begin(), found);
      throw std::runtime_error("conflicting embedded descriptors for group '" +
                               record.descriptor.group_id + "' at offsets " +
                               std::to_string(offsets[static_cast<std::size_t>(conflict)]) +
                               " and " + std::to_string(record.offset));
    }
    // An identical duplicate is the same declaration linked twice; keep one.
  }
  return resolved;
}

} // namespace neko::detail
