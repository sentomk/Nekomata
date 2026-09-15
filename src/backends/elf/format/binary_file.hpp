#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neko::elf {

enum class binary_kind : std::uint8_t { executable, relocatable };

struct address_range {
  std::uint64_t begin = 0;
  std::uint64_t end = 0; // exclusive, link-time virtual addresses
};

struct function_symbol {
  // Identity within this file, not across rebuilds. Never keyed by name alone.
  std::uint32_t table_section = 0;
  std::uint64_t table_index = 0;
  std::uint32_t section = 0;
  std::string name;
  // Link-time virtual address for ET_EXEC, offset within section for ET_REL.
  // An offset alone never identifies code in a relocatable object.
  std::uint64_t address = 0;
  std::uint64_t size = 0; // zero means unknown; do not infer an end address
  std::uint8_t binding = 0;
  std::uint8_t visibility = 0;
};

struct function_symbols {
  bool has_symtab = false;
  std::vector<function_symbol> functions;
  binary_kind kind = binary_kind::executable;
};

struct debug_relocation {
  std::uint32_t relocation_section = 0;
  std::uint64_t relocation_index = 0;
  std::uint64_t offset = 0; // Field offset within .debug_info.
  std::uint32_t type = 0;
  std::uint8_t width = 0;
  std::uint32_t symbol_table = 0;
  std::uint64_t symbol_index = 0;
  std::uint32_t symbol_section = 0;
  std::uint64_t symbol_value = 0;
  std::int64_t addend = 0;
  // S + A, relative to symbol_section, never a process/virtual address.
  // May equal the section size (e.g. an exclusive high_pc endpoint).
  std::uint64_t resolved_offset = 0;
};

struct debug_relocations {
  // Missing .debug_info differs from a present section with no relocations.
  std::optional<std::uint32_t> info_section;
  // Sorted by field offset; overlapping/composed relocations are rejected.
  std::vector<debug_relocation> entries;
};

// Read-only ELF64 little-endian x86-64 ET_EXEC or ET_REL input. Keep this descriptor
// open for both ELF and DWARF reads so replacing the path cannot mix files.
// The caller must not modify the opened file in place during inspection.
class binary_file {
public:
  explicit binary_file(const std::filesystem::path& path);
  ~binary_file();
  binary_file(const binary_file&) = delete;
  binary_file& operator=(const binary_file&) = delete;

  int fd() const { return fd_; }
  const std::filesystem::path& path() const { return path_; }
  binary_kind kind() const { return kind_; }
  bool contains(std::uint64_t offset, std::uint64_t size) const;
  void read(std::uint64_t offset, std::span<std::uint8_t> out) const;
  // ET_EXEC only. ET_REL has section-relative code, not load segments.
  std::vector<address_range> executable_ranges() const;
  // Only defined STT_FUNC entries in SHT_SYMTAB, not duplicated .dynsym entries.
  // Missing .symtab is represented explicitly; malformed tables throw.
  function_symbols symbols() const;
  // ET_REL only, uncompressed/ungrouped .debug_info, RELA absolute 32/64.
  // Validates relocation metadata and referenced symbols without reading or
  // modifying DWARF contents. Does not enable DWARF inspection or patching.
  debug_relocations debug_info_relocations() const;

private:
  void validate_header();
  std::filesystem::path path_;
  int fd_ = -1;
  std::uint64_t size_ = 0;
  binary_kind kind_ = binary_kind::executable;
};

} // namespace neko::elf
