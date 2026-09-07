#include "binary_file.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace neko::elf {
namespace {

std::uint64_t number(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  }
  return value;
}

template <std::size_t Size>
std::array<std::uint8_t, Size> read_entry(const binary_file& file, std::uint64_t offset) {
  std::array<std::uint8_t, Size> bytes{};
  file.read(offset, bytes);
  return bytes;
}

struct section {
  std::uint64_t type = 0, flags = 0, address = 0, offset = 0, size = 0;
  std::uint64_t link = 0, info = 0, entry_size = 0;
  std::uint64_t name = 0;
};

std::vector<section> sections(const binary_file& file) {
  const auto header = read_entry<sizeof(Elf64_Ehdr)>(file, 0);
  const std::span<const std::uint8_t> bytes(header);
  const auto offset = number(bytes.subspan(40, 8));
  const auto count = number(bytes.subspan(60, 2));
  if (offset == 0 && count == 0) {
    return {};
  }
  if (offset == 0 || count == 0 || count >= SHN_LORESERVE ||
      number(bytes.subspan(58, 2)) != sizeof(Elf64_Shdr) ||
      !file.contains(offset, count * sizeof(Elf64_Shdr))) {
    throw std::runtime_error("invalid or unsupported ELF section table");
  }
  std::vector<section> out;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto raw = read_entry<sizeof(Elf64_Shdr)>(file, offset + i * sizeof(Elf64_Shdr));
    const std::span<const std::uint8_t> entry(raw);
    section value{
        number(entry.subspan(4, 4)),  number(entry.subspan(8, 8)),  number(entry.subspan(16, 8)),
        number(entry.subspan(24, 8)), number(entry.subspan(32, 8)), number(entry.subspan(40, 4)),
        number(entry.subspan(44, 4)), number(entry.subspan(56, 8)), number(entry.subspan(0, 4))};
    if (value.type != SHT_NOBITS && !file.contains(value.offset, value.size)) {
      throw std::runtime_error("ELF section is outside the file");
    }
    out.push_back(value);
  }
  return out;
}

constexpr std::uint64_t kRelocationLimit = 1000000;
constexpr std::uint64_t kSymbolLimit = 1000000;
constexpr std::uint64_t kStringLimit = 64 * 1024 * 1024;

std::vector<std::uint8_t> read_strings(const binary_file& file, const section& value,
                                       std::uint64_t& budget_used) {
  if (value.type != SHT_STRTAB || (value.flags & SHF_COMPRESSED) || value.size == 0 ||
      value.size > kStringLimit - budget_used) {
    throw std::runtime_error("invalid or oversized relocation string table");
  }
  budget_used += value.size;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(value.size));
  file.read(value.offset, bytes);
  if (bytes.front() != 0 || bytes.back() != 0) {
    throw std::runtime_error("relocation string table must start and end with NUL");
  }
  return bytes;
}

bool named(std::span<const std::uint8_t> strings, std::uint64_t offset, std::string_view name) {
  // Fixed-size comparisons avoid repeatedly scanning enormous aliased names.
  return name.size() < strings.size() - offset &&
         std::memcmp(strings.data() + offset, name.data(), name.size()) == 0 &&
         strings[static_cast<std::size_t>(offset) + name.size()] == 0;
}

struct relocation_symbols {
  std::vector<std::uint8_t> bytes;
  std::vector<std::uint8_t> strings;
};

relocation_symbols read_relocation_symbols(const binary_file& file,
                                           const std::vector<section>& table, std::uint64_t index,
                                           std::uint64_t& symbols_used,
                                           std::uint64_t& strings_used) {
  if (index >= table.size()) {
    throw std::runtime_error("invalid relocation symbol table index");
  }
  const auto& value = table[index];
  if (value.type != SHT_SYMTAB || (value.flags & SHF_COMPRESSED) ||
      value.entry_size != sizeof(Elf64_Sym) || value.size == 0 ||
      value.size % sizeof(Elf64_Sym) != 0 || value.link >= table.size() ||
      value.info > value.size / sizeof(Elf64_Sym)) {
    throw std::runtime_error("invalid relocation symbol table metadata");
  }
  const auto count = value.size / sizeof(Elf64_Sym);
  if (count > kSymbolLimit - symbols_used) {
    throw std::runtime_error("relocation symbol tables exceed inspection limits");
  }
  symbols_used += count;
  relocation_symbols out;
  out.strings = read_strings(file, table[value.link], strings_used);
  out.bytes.resize(static_cast<std::size_t>(value.size));
  file.read(value.offset, out.bytes);
  if (std::any_of(out.bytes.begin(), out.bytes.begin() + sizeof(Elf64_Sym),
                  [](auto byte) { return byte != 0; })) {
    throw std::runtime_error("invalid relocation null symbol");
  }
  return out;
}

std::uint64_t add_offset(std::uint64_t value, std::int64_t addend) {
  if (addend < 0) {
    const auto magnitude = static_cast<std::uint64_t>(-(addend + 1)) + 1;
    if (magnitude > value) {
      throw std::runtime_error("debug relocation offset underflow");
    }
    return value - magnitude;
  }
  const auto positive = static_cast<std::uint64_t>(addend);
  if (positive > std::numeric_limits<std::uint64_t>::max() - value) {
    throw std::runtime_error("debug relocation offset overflow");
  }
  return value + positive;
}

} // namespace

binary_file::binary_file(const std::filesystem::path& path)
    : path_(std::filesystem::absolute(path).lexically_normal()) {
  fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd_ < 0) {
    throw std::runtime_error("cannot open binary: " + std::string(std::strerror(errno)));
  }
  try {
    struct stat status {};
    if (fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
      throw std::runtime_error("binary must be a readable regular file");
    }
    size_ = static_cast<std::uint64_t>(status.st_size);
    // Validate before exposing a descriptor to another reader.
    validate_header();
  } catch (...) {
    close(fd_);
    throw;
  }
}

binary_file::~binary_file() {
  close(fd_);
}

bool binary_file::contains(std::uint64_t offset, std::uint64_t size) const {
  return offset <= size_ && size <= size_ - offset;
}

void binary_file::read(std::uint64_t offset, std::span<std::uint8_t> out) const {
  if (!contains(offset, out.size())) {
    throw std::runtime_error("truncated ELF data");
  }
  std::size_t done = 0;
  while (done < out.size()) {
    const auto count =
        pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error("failed to read ELF binary");
    }
    done += static_cast<std::size_t>(count);
  }
}

void binary_file::validate_header() {
  const auto header = read_entry<sizeof(Elf64_Ehdr)>(*this, 0);
  if (std::memcmp(header.data(), ELFMAG, SELFMAG) != 0 || header[EI_CLASS] != ELFCLASS64 ||
      header[EI_DATA] != ELFDATA2LSB || header[EI_VERSION] != EV_CURRENT) {
    throw std::runtime_error("expected an ELF64 little-endian binary");
  }
  const std::span<const std::uint8_t> bytes(header);
  if (number(bytes.subspan(18, 2)) != EM_X86_64) {
    throw std::runtime_error("only x86-64 ELF inspection is supported");
  }
  const auto type = number(bytes.subspan(16, 2));
  if (type != ET_EXEC && type != ET_REL) {
    throw std::runtime_error(
        "expected ET_EXEC or ET_REL; PIE and shared libraries are unsupported");
  }
  if (number(bytes.subspan(20, 4)) != EV_CURRENT ||
      number(bytes.subspan(52, 2)) != sizeof(Elf64_Ehdr)) {
    throw std::runtime_error("invalid ELF header sizes or version");
  }
  kind_ = type == ET_REL ? binary_kind::relocatable : binary_kind::executable;
  if (kind_ == binary_kind::relocatable) {
    const auto entry_size = number(bytes.subspan(54, 2));
    if (number(bytes.subspan(32, 8)) != 0 || number(bytes.subspan(56, 2)) != 0 ||
        (entry_size != 0 && entry_size != sizeof(Elf64_Phdr))) {
      throw std::runtime_error("ET_REL with a segment table is unsupported");
    }
    const auto table = sections(*this);
    if (table.empty()) {
      throw std::runtime_error("ET_REL requires a section table");
    }
    // No load addresses have been assigned yet. Do not silently mix address
    // models if a producer supplies an unusual relocatable layout.
    if (std::any_of(table.begin(), table.end(),
                    [](const auto& value) { return value.address != 0; })) {
      throw std::runtime_error("ET_REL sections must have zero virtual addresses");
    }
    return;
  }
  static_cast<void>(executable_ranges());
}

std::vector<address_range> binary_file::executable_ranges() const {
  if (kind_ != binary_kind::executable) {
    throw std::runtime_error("ET_REL uses section offsets, not executable load segments");
  }
  const auto header = read_entry<sizeof(Elf64_Ehdr)>(*this, 0);
  const std::span<const std::uint8_t> bytes(header);
  if (number(bytes.subspan(54, 2)) != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("invalid ELF segment entry size");
  }
  const auto offset = number(bytes.subspan(32, 8));
  const auto count = number(bytes.subspan(56, 2));
  if (count == 0 || count == PN_XNUM || !contains(offset, count * sizeof(Elf64_Phdr))) {
    throw std::runtime_error("missing, truncated or unsupported ELF segment table");
  }
  std::vector<address_range> ranges;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto raw = read_entry<sizeof(Elf64_Phdr)>(*this, offset + i * sizeof(Elf64_Phdr));
    const std::span<const std::uint8_t> entry(raw);
    if (number(entry.subspan(0, 4)) != PT_LOAD || !(number(entry.subspan(4, 4)) & PF_X)) {
      continue;
    }
    const auto begin = number(entry.subspan(16, 8));
    const auto size = number(entry.subspan(32, 8));
    if (!contains(number(entry.subspan(8, 8)), size) || size > number(entry.subspan(40, 8)) ||
        size > std::numeric_limits<std::uint64_t>::max() - begin) {
      throw std::runtime_error("invalid ELF executable segment range");
    }
    ranges.push_back({begin, begin + size});
  }
  if (ranges.empty()) {
    throw std::runtime_error("ELF has no executable load segment");
  }
  return ranges;
}

debug_relocations binary_file::debug_info_relocations() const {
  if (kind_ != binary_kind::relocatable) {
    throw std::runtime_error("debug relocations require ET_REL input");
  }
  const auto table = sections(*this);
  const auto header = read_entry<sizeof(Elf64_Ehdr)>(*this, 0);
  const auto names_index = number(std::span<const std::uint8_t>(header).subspan(62, 2));
  if (names_index == SHN_UNDEF || names_index >= table.size()) {
    throw std::runtime_error("missing or unsupported ELF section-name table");
  }
  std::uint64_t strings_used = 0, symbols_used = 0, relocations_used = 0;
  const auto names = read_strings(*this, table[names_index], strings_used);
  debug_relocations out;
  for (std::size_t i = 0; i < table.size(); ++i) {
    const auto& value = table[i];
    if (value.name >= names.size()) {
      throw std::runtime_error("invalid ELF section name index");
    }
    if (named(names, value.name, ".zdebug_info") || named(names, value.name, ".debug_info.dwo")) {
      throw std::runtime_error("compressed or split debug relocations are unsupported");
    }
    if (!named(names, value.name, ".debug_info")) {
      continue;
    }
    if (out.info_section || value.type != SHT_PROGBITS ||
        (value.flags & (SHF_COMPRESSED | SHF_GROUP | SHF_ALLOC))) {
      throw std::runtime_error("duplicate, compressed, grouped or invalid .debug_info section");
    }
    out.info_section = static_cast<std::uint32_t>(i);
  }
  if (!out.info_section) {
    return out;
  }
  std::map<std::uint64_t, relocation_symbols> symbol_tables;
  for (std::size_t i = 0; i < table.size(); ++i) {
    const auto& relocations = table[i];
    if (relocations.type != SHT_RELA && relocations.type != SHT_REL) {
      continue;
    }
    if (relocations.info == 0 || relocations.info >= table.size()) {
      throw std::runtime_error("invalid relocation target section index");
    }
    if (relocations.info != *out.info_section) {
      continue; // Code/other debug relocations are outside this reader's scope.
    }
    if (relocations.type != SHT_RELA || (relocations.flags & (SHF_COMPRESSED | SHF_GROUP)) ||
        relocations.entry_size != sizeof(Elf64_Rela) ||
        relocations.size % sizeof(Elf64_Rela) != 0) {
      throw std::runtime_error("unsupported or malformed .debug_info relocation table");
    }
    const auto count = relocations.size / sizeof(Elf64_Rela);
    if (count > kRelocationLimit - relocations_used) {
      throw std::runtime_error("debug relocations exceed inspection limits");
    }
    relocations_used += count;
    auto found = symbol_tables.find(relocations.link);
    if (found == symbol_tables.end()) {
      found = symbol_tables
                  .emplace(relocations.link, read_relocation_symbols(*this, table, relocations.link,
                                                                     symbols_used, strings_used))
                  .first;
    }
    const auto& symbols = found->second;
    for (std::uint64_t r = 0; r < count; ++r) {
      const auto raw =
          read_entry<sizeof(Elf64_Rela)>(*this, relocations.offset + r * sizeof(Elf64_Rela));
      const std::span<const std::uint8_t> entry(raw);
      debug_relocation value;
      value.relocation_section = static_cast<std::uint32_t>(i);
      value.relocation_index = r;
      value.offset = number(entry.first(8));
      const auto info = number(entry.subspan(8, 8));
      value.type = static_cast<std::uint32_t>(ELF64_R_TYPE(info));
      if (value.type != R_X86_64_32 && value.type != R_X86_64_64) {
        throw std::runtime_error("unsupported .debug_info relocation type");
      }
      value.width = value.type == R_X86_64_32 ? 4 : 8;
      const auto info_size = table[*out.info_section].size;
      if (value.offset > info_size || value.width > info_size - value.offset) {
        throw std::runtime_error("debug relocation field is outside .debug_info");
      }
      value.symbol_table = static_cast<std::uint32_t>(relocations.link);
      value.symbol_index = ELF64_R_SYM(info);
      if (value.symbol_index == 0 ||
          value.symbol_index >= symbols.bytes.size() / sizeof(Elf64_Sym)) {
        throw std::runtime_error("invalid debug relocation symbol index");
      }
      const auto symbol =
          std::span<const std::uint8_t>(symbols.bytes)
              .subspan(static_cast<std::size_t>(value.symbol_index) * sizeof(Elf64_Sym),
                       sizeof(Elf64_Sym));
      value.symbol_section = static_cast<std::uint32_t>(number(symbol.subspan(6, 2)));
      if (number(symbol.first(4)) >= symbols.strings.size() || value.symbol_section == SHN_UNDEF ||
          value.symbol_section >= SHN_LORESERVE || value.symbol_section >= table.size()) {
        throw std::runtime_error("undefined, special or invalid debug relocation symbol");
      }
      const auto type = ELF64_ST_TYPE(symbol[4]);
      const auto& target = table[value.symbol_section];
      if ((type != STT_SECTION && type != STT_NOTYPE && type != STT_FUNC && type != STT_OBJECT) ||
          (target.type != SHT_PROGBITS && target.type != SHT_NOBITS) ||
          (target.flags & SHF_COMPRESSED)) {
        throw std::runtime_error("unsupported debug relocation symbol target");
      }
      value.symbol_value = number(symbol.subspan(8, 8));
      const auto size = number(symbol.subspan(16, 8));
      if (value.symbol_value > target.size || size > target.size - value.symbol_value) {
        throw std::runtime_error("debug relocation symbol is outside its section");
      }
      value.addend = std::bit_cast<std::int64_t>(number(entry.subspan(16, 8)));
      value.resolved_offset = add_offset(value.symbol_value, value.addend);
      if (value.type == R_X86_64_32 &&
          value.resolved_offset > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("debug relocation value does not fit 32 bits");
      }
      if (value.resolved_offset > target.size) {
        throw std::runtime_error("debug relocation offset is outside its section");
      }
      out.entries.push_back(value);
    }
  }
  std::sort(out.entries.begin(), out.entries.end(),
            [](const auto& left, const auto& right) { return left.offset < right.offset; });
  for (std::size_t i = 1; i < out.entries.size(); ++i) {
    const auto& previous = out.entries[i - 1];
    if (out.entries[i].offset - previous.offset < previous.width) {
      throw std::runtime_error("overlapping or composed debug relocations are unsupported");
    }
  }
  return out;
}

function_symbols binary_file::symbols() const {
  const auto table = sections(*this);
  const bool relocatable = kind_ == binary_kind::relocatable;
  const auto executable = relocatable ? std::vector<address_range>{} : executable_ranges();
  function_symbols out;
  out.kind = kind_;
  constexpr std::uint64_t max_strings = 64 * 1024 * 1024;
  constexpr std::uint64_t max_symbols = 1000000;
  std::uint64_t string_bytes = 0, name_bytes = 0, symbol_count = 0;
  for (std::size_t t = 0; t < table.size(); ++t) {
    const auto& symtab = table[t];
    if (symtab.type != SHT_SYMTAB) {
      continue;
    }
    out.has_symtab = true;
    if (symtab.entry_size != sizeof(Elf64_Sym) || symtab.size == 0 ||
        symtab.size % sizeof(Elf64_Sym) != 0 || symtab.link >= table.size() ||
        table[symtab.link].type != SHT_STRTAB || symtab.info > symtab.size / sizeof(Elf64_Sym)) {
      throw std::runtime_error("invalid ELF symbol table metadata");
    }
    const auto& strtab = table[symtab.link];
    // Bound allocations/work even for deliberately enormous sparse inputs.
    const auto count = symtab.size / sizeof(Elf64_Sym);
    if (strtab.size == 0 || strtab.size > max_strings - string_bytes ||
        count > max_symbols - symbol_count) {
      throw std::runtime_error("ELF symbol table exceeds inspection limits");
    }
    string_bytes += strtab.size;
    symbol_count += count;
    std::vector<std::uint8_t> strings(static_cast<std::size_t>(strtab.size));
    read(strtab.offset, strings);
    if (strings.front() != 0 || strings.back() != 0) {
      throw std::runtime_error("ELF string table must start and end with NUL");
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto raw = read_entry<sizeof(Elf64_Sym)>(*this, symtab.offset + i * sizeof(Elf64_Sym));
      const std::span<const std::uint8_t> entry(raw);
      if (i == 0) {
        if (std::any_of(raw.begin(), raw.end(), [](auto byte) { return byte != 0; })) {
          throw std::runtime_error("invalid ELF null symbol");
        }
        continue;
      }
      const auto name = number(entry.subspan(0, 4));
      const auto section_index = number(entry.subspan(6, 2));
      if (name >= strings.size() || section_index == SHN_XINDEX ||
          (section_index < SHN_LORESERVE && section_index >= table.size())) {
        throw std::runtime_error("invalid or unsupported ELF symbol name/section index");
      }
      if (ELF64_ST_TYPE(raw[4]) != STT_FUNC || section_index == SHN_UNDEF) {
        continue;
      }
      if (section_index >= SHN_LORESERVE) {
        throw std::runtime_error("unsupported special section for ELF function");
      }
      const auto address = number(entry.subspan(8, 8));
      const auto size = number(entry.subspan(16, 8));
      const auto& code = table[section_index];
      const auto base = relocatable ? 0 : code.address;
      if (code.type == SHT_NOBITS || !(code.flags & SHF_EXECINSTR) || !(code.flags & SHF_ALLOC) ||
          address < base || address - base >= code.size || size > code.size - (address - base) ||
          (!relocatable &&
           !std::any_of(executable.begin(), executable.end(), [&](const auto& range) {
             return address >= range.begin && address < range.end && size <= range.end - address;
           }))) {
        throw std::runtime_error("ELF function is outside executable code");
      }
      const auto first = strings.begin() + static_cast<std::ptrdiff_t>(name);
      // The same long name may be referenced by many symbols. Bound both the
      // scan and the owned copies, not just the input string table allocation.
      const auto scan_size = std::min(strtab.size - name, max_strings - name_bytes + 1);
      const auto scan_end = first + static_cast<std::ptrdiff_t>(scan_size);
      const auto last = std::find(first, scan_end, std::uint8_t{0});
      if (last == scan_end) {
        throw std::runtime_error("ELF function names exceed inspection limits");
      }
      name_bytes += static_cast<std::uint64_t>(last - first);
      out.functions.push_back({static_cast<std::uint32_t>(t), i,
                               static_cast<std::uint32_t>(section_index), std::string(first, last),
                               address, size, static_cast<std::uint8_t>(ELF64_ST_BIND(raw[4])),
                               static_cast<std::uint8_t>(ELF64_ST_VISIBILITY(raw[5]))});
    }
  }
  return out;
}

} // namespace neko::elf
