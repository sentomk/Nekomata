#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace neko::elf {

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
  std::uint64_t address = 0;
  std::uint64_t size = 0; // zero means unknown; do not infer an end address
  std::uint8_t binding = 0;
  std::uint8_t visibility = 0;
};

struct function_symbols {
  bool has_symtab = false;
  std::vector<function_symbol> functions;
};

// Read-only ELF64 little-endian x86-64 ET_EXEC input. Keep this descriptor
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
  bool contains(std::uint64_t offset, std::uint64_t size) const;
  void read(std::uint64_t offset, std::span<std::uint8_t> out) const;
  std::vector<address_range> executable_ranges() const;
  // Only defined STT_FUNC entries in SHT_SYMTAB, not duplicated .dynsym entries.
  // Missing .symtab is represented explicitly; malformed tables throw.
  function_symbols symbols() const;

private:
  std::filesystem::path path_;
  int fd_ = -1;
  std::uint64_t size_ = 0;
};

} // namespace neko::elf
