#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neko::elf {
class binary_file;
}

namespace neko::dwarf {

// Link-time virtual addresses, not file offsets or live process addresses.
struct address_range {
  std::uint64_t begin = 0;
  std::uint64_t end = 0; // exclusive
};

struct function_record {
  // A DIE in this binary's .debug_info; NOT an identity across rebuilds.
  std::uint64_t die_offset = 0;
  std::string name;
  std::optional<std::string> linkage_name;
  std::vector<address_range> ranges;
};

struct compilation_unit {
  std::uint64_t die_offset = 0;
  std::string name;
  std::string directory;
  std::string producer;
  std::vector<function_record> functions;
  // Abstract definitions and other DIEs without emitted code are reported,
  // never assigned an address by guessing from their name.
  std::vector<std::string> unlocated_functions;
};

struct binary_info {
  std::filesystem::path path;
  std::vector<compilation_unit> units;
};

// Read-only, Linux ELF64 x86-64 ET_EXEC, embedded DWARF 4 / DWARF32.
// Throws std::runtime_error on malformed, missing, or unsupported information.
// Records describe one file only; this is not a cross-version matching API.
binary_info inspect(const std::filesystem::path& path);
// Internal overload for ELF/DWARF inspection through one open descriptor.
binary_info inspect(const elf::binary_file& file);

} // namespace neko::dwarf
