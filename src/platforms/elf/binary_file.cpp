#include "binary_file.hpp"

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
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
    section value{number(entry.subspan(4, 4)),  number(entry.subspan(8, 8)),
                  number(entry.subspan(16, 8)), number(entry.subspan(24, 8)),
                  number(entry.subspan(32, 8)), number(entry.subspan(40, 4)),
                  number(entry.subspan(44, 4)), number(entry.subspan(56, 8))};
    if (value.type != SHT_NOBITS && !file.contains(value.offset, value.size)) {
      throw std::runtime_error("ELF section is outside the file");
    }
    out.push_back(value);
  }
  return out;
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
    static_cast<void>(executable_ranges());
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

std::vector<address_range> binary_file::executable_ranges() const {
  const auto header = read_entry<sizeof(Elf64_Ehdr)>(*this, 0);
  if (std::memcmp(header.data(), ELFMAG, SELFMAG) != 0 || header[EI_CLASS] != ELFCLASS64 ||
      header[EI_DATA] != ELFDATA2LSB || header[EI_VERSION] != EV_CURRENT) {
    throw std::runtime_error("expected an ELF64 little-endian binary");
  }
  const std::span<const std::uint8_t> bytes(header);
  if (number(bytes.subspan(18, 2)) != EM_X86_64) {
    throw std::runtime_error("only x86-64 ELF inspection is supported");
  }
  if (number(bytes.subspan(16, 2)) != ET_EXEC) {
    throw std::runtime_error(
        "expected ET_EXEC; PIE, shared libraries and .o files are unsupported");
  }
  if (number(bytes.subspan(20, 4)) != EV_CURRENT ||
      number(bytes.subspan(52, 2)) != sizeof(Elf64_Ehdr) ||
      number(bytes.subspan(54, 2)) != sizeof(Elf64_Phdr)) {
    throw std::runtime_error("invalid ELF header sizes or version");
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

function_symbols binary_file::symbols() const {
  const auto table = sections(*this);
  const auto executable = executable_ranges();
  function_symbols out;
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
    constexpr std::uint64_t max_strings = 64 * 1024 * 1024;
    constexpr std::uint64_t max_symbols = 1000000;
    const auto count = symtab.size / sizeof(Elf64_Sym);
    if (strtab.size == 0 || strtab.size > max_strings || count > max_symbols) {
      throw std::runtime_error("ELF symbol table exceeds inspection limits");
    }
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
      if (code.type == SHT_NOBITS || !(code.flags & SHF_EXECINSTR) || !(code.flags & SHF_ALLOC) ||
          address < code.address || address - code.address >= code.size ||
          size > code.size - (address - code.address) ||
          !std::any_of(executable.begin(), executable.end(), [&](const auto& range) {
            return address >= range.begin && address < range.end && size <= range.end - address;
          })) {
        throw std::runtime_error("ELF function is outside executable code");
      }
      const auto first = strings.begin() + static_cast<std::ptrdiff_t>(name);
      const auto last = std::find(first, strings.end(), std::uint8_t{0});
      out.functions.push_back({static_cast<std::uint32_t>(t), i,
                               static_cast<std::uint32_t>(section_index), std::string(first, last),
                               address, size, static_cast<std::uint8_t>(ELF64_ST_BIND(raw[4])),
                               static_cast<std::uint8_t>(ELF64_ST_VISIBILITY(raw[5]))});
    }
  }
  return out;
}

} // namespace neko::elf
