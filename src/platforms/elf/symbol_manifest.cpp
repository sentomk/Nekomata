#include "symbol_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace neko::elf {
namespace {

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::uint64_t parse_address(std::string_view field) {
  if (field.size() < 3 || field[0] != '0' || (field[1] != 'x' && field[1] != 'X')) {
    throw std::runtime_error("symbol manifest: address is not hexadecimal: '" + std::string(field) +
                             "'");
  }
  std::uint64_t value = 0;
  for (const char digit : field.substr(2)) {
    const int nibble = digit >= '0' && digit <= '9'   ? digit - '0'
                       : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                       : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                                                      : -1;
    if (nibble < 0) {
      throw std::runtime_error("symbol manifest: address is not hexadecimal: '" +
                               std::string(field) + "'");
    }
    value = (value << 4) | static_cast<std::uint64_t>(nibble);
  }
  return value;
}

std::filesystem::path normalized_source(std::string_view source) {
  return std::filesystem::path(source).lexically_normal();
}

} // namespace

symbol_manifest symbol_manifest::parse(std::string_view text) {
  symbol_manifest manifest;
  std::size_t line_number = 0;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    const std::string_view line = trim(text.substr(0, newline));
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    ++line_number;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto first_tab = line.find('\t');
    const auto second_tab = first_tab == std::string_view::npos ? std::string_view::npos
                                                                : line.find('\t', first_tab + 1);
    if (first_tab == std::string_view::npos || second_tab == std::string_view::npos) {
      throw std::runtime_error("symbol manifest: line " + std::to_string(line_number) +
                               " is not '<address>\\t<symbol>\\t<source>'");
    }
    manifest_entry entry;
    entry.address = parse_address(line.substr(0, first_tab));
    entry.symbol = std::string(line.substr(first_tab + 1, second_tab - first_tab - 1));
    entry.source = std::string(line.substr(second_tab + 1));
    if (entry.symbol.empty() || entry.source.empty()) {
      throw std::runtime_error("symbol manifest: line " + std::to_string(line_number) +
                               " has an empty symbol or source");
    }
    manifest.entries_.push_back(std::move(entry));
  }
  return manifest;
}

symbol_manifest symbol_manifest::load(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream text;
  text << file.rdbuf();
  return parse(text.str());
}

symbol_manifest symbol_manifest::discover(const std::filesystem::path& executable) {
  // An empty NEKOMATA_MANIFEST means "do not look": that is how a test or a
  // cautious deployment turns the whole mechanism off.
  if (const char* explicit_path = std::getenv("NEKOMATA_MANIFEST")) {
    return load(explicit_path);
  }
  if (executable.empty()) {
    return {};
  }
  // `/proc/self/exe` is the path the runtime uses to read its own ELF image,
  // but the manifest lives beside the actual executable. Resolve that symlink
  // before adding the suffix; `/proc/self/exe.nekomata-map` is a different,
  // nonexistent procfs entry.
  std::error_code ec;
  const auto resolved = std::filesystem::canonical(executable, ec);
  const auto& manifest_host = ec ? executable : resolved;
  return load(manifest_host.string() + ".nekomata-map");
}

bool symbol_manifest::contains_source(std::string_view source_path) const {
  if (source_path.empty()) {
    return false;
  }
  const auto wanted_source = normalized_source(source_path);
  return std::any_of(entries_.begin(), entries_.end(), [&](const manifest_entry& entry) {
    return normalized_source(entry.source) == wanted_source;
  });
}

std::optional<std::uint64_t>
symbol_manifest::address_in_source(std::string_view symbol, std::string_view source_path) const {
  if (entries_.empty() || source_path.empty()) {
    return std::nullopt;
  }

  const auto wanted_source = normalized_source(source_path);
  const std::string key(symbol);
  std::optional<std::uint64_t> found;
  for (const auto& entry : entries_) {
    if (normalized_source(entry.source) != wanted_source || entry.symbol != key) {
      continue;
    }
    if (found.has_value()) {
      return std::nullopt; // two addresses for one name in one unit
    }
    found = entry.address;
  }
  return found;
}

} // namespace neko::elf
