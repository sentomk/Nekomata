#include "object_file.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace neko::pe {
namespace {

// Every supported host is little-endian; the ELF reader relies on the same
// assumption through its reinterpret_casts.
struct coff_header {
  std::uint16_t machine;
  std::uint16_t section_count;
  std::uint32_t timestamp;
  std::uint32_t symbol_table_offset;
  std::uint32_t symbol_count;
  std::uint16_t optional_header_size;
  std::uint16_t characteristics;
};
static_assert(sizeof(coff_header) == 20);

struct coff_section_header {
  char name[8];
  std::uint32_t virtual_size;
  std::uint32_t virtual_address;
  std::uint32_t raw_data_size;
  std::uint32_t raw_data_offset;
  std::uint32_t relocation_offset;
  std::uint32_t line_number_offset;
  std::uint16_t relocation_count;
  std::uint16_t line_number_count;
  std::uint32_t characteristics;
};
static_assert(sizeof(coff_section_header) == 40);

constexpr std::uint16_t machine_amd64 = 0x8664;

// Fixed COFF symbol record size; the string table follows the last record.
constexpr std::size_t coff_symbol_size = 18;

constexpr std::uint32_t flag_cnt_code = 0x00000020;
constexpr std::uint32_t flag_cnt_initialized = 0x00000040;
constexpr std::uint32_t flag_cnt_uninitialized = 0x00000080;
constexpr std::uint32_t flag_link_info = 0x00000200;
constexpr std::uint32_t flag_link_remove = 0x00000800;
constexpr std::uint32_t flag_comdat = 0x00001000;
constexpr std::uint32_t flag_mem_execute = 0x20000000;
constexpr std::uint32_t flag_mem_write = 0x80000000;
constexpr std::uint32_t align_shift = 20;

const std::uint8_t* at(const std::uint8_t* base, std::size_t size, std::uint64_t offset,
                       std::uint64_t bytes, const char* what) {
  // Overflow-proof bounds check: the additive form (offset + bytes > size)
  // wraps around uint64 on crafted headers and lets huge offsets through.
  // The subtractive form cannot wrap.
  if (bytes > size || offset > size - bytes) {
    throw std::runtime_error(std::string("truncated object file: ") + what + " out of bounds");
  }
  return base + offset;
}

bool never_loaded(const std::string& name) {
  // Unwind tables and CodeView streams carry image-relative data that cannot
  // move into the arena; flags alone cannot tell them from read-only data.
  return name == ".pdata" || name == ".xdata" || name.starts_with(".debug$");
}

section_class classify(const std::string& name, std::uint32_t flags) {
  if (name.empty() || (flags & (flag_link_info | flag_link_remove)) != 0 || never_loaded(name)) {
    return section_class::other;
  }
  if ((flags & flag_cnt_code) != 0 && (flags & flag_mem_execute) != 0) {
    return section_class::text;
  }
  if ((flags & flag_cnt_uninitialized) != 0) {
    // Storage the loader sizes but never reads from the file, like SHT_NOBITS.
    return section_class::data;
  }
  if ((flags & flag_cnt_initialized) != 0) {
    return (flags & flag_mem_write) != 0 ? section_class::data : section_class::rodata;
  }
  return section_class::other;
}

std::uint64_t decimal_offset(const char* begin, const char* end) {
  std::uint64_t value = 0;
  if (begin == end) {
    throw std::runtime_error("section name offset is empty");
  }
  for (const char* p = begin; p != end; ++p) {
    if (*p < '0' || *p > '9') {
      throw std::runtime_error("section name offset is not decimal");
    }
    value = value * 10 + static_cast<std::uint8_t>(*p - '0');
  }
  return value;
}

// The eight-byte name field holds either the name (NUL-padded, truncation at
// eight bytes is legal) or "/ddd": a decimal offset into the string table
// that follows the symbol table. clang emits such names (.llvm_addrsig).
std::string section_name(const coff_section_header& raw, const std::uint8_t* data, std::size_t size,
                         const coff_header& header) {
  if (raw.name[0] != '/') {
    const char* end = std::find(raw.name, raw.name + sizeof(raw.name), '\0');
    return std::string(raw.name, end);
  }
  if (header.symbol_table_offset == 0) {
    throw std::runtime_error("long section name without a symbol table");
  }
  const std::uint64_t digits_end =
      decimal_offset(raw.name + 1, std::find(raw.name + 1, raw.name + sizeof(raw.name), '\0'));
  const std::uint64_t table = static_cast<std::uint64_t>(header.symbol_table_offset) +
                              static_cast<std::uint64_t>(header.symbol_count) * coff_symbol_size;
  const std::uint8_t* start = at(data, size, table + digits_end, 1, "section name in string table");
  const void* nul = std::memchr(start, 0, size - (static_cast<std::size_t>(start - data)));
  if (nul == nullptr) {
    throw std::runtime_error("truncated object file: section name is not terminated");
  }
  return std::string(reinterpret_cast<const char*>(start),
                     static_cast<std::size_t>(static_cast<const char*>(nul) -
                                              reinterpret_cast<const char*>(start)));
}

} // namespace

object_file parse_object(const std::uint8_t* data, std::size_t size) {
  auto require = [](bool ok, const char* what) {
    if (!ok) {
      throw std::runtime_error(std::string("not a supported object file: ") + what);
    }
  };

  const auto* header =
      reinterpret_cast<const coff_header*>(at(data, size, 0, sizeof(coff_header), "COFF header"));
  // Sig1 == IMAGE_FILE_MACHINE_UNKNOWN distinguishes the bigobj header from
  // a plain machine type.
  require(header->machine != 0, "bigobj objects are not supported yet");
  require(header->machine == machine_amd64, "not x86-64");
  require(header->optional_header_size == 0, "not an object file (has an optional header)");
  require(header->section_count > 0, "no sections");

  // No optional header, so the section table starts right after the header.
  const std::uint64_t table_offset = sizeof(coff_header);
  object_file obj;
  obj.sections.reserve(header->section_count);
  for (std::uint16_t i = 0; i < header->section_count; ++i) {
    const auto* raw = reinterpret_cast<const coff_section_header*>(
        at(data, size, table_offset + static_cast<std::uint64_t>(i) * sizeof(coff_section_header),
           sizeof(coff_section_header), "section header"));

    section sec;
    sec.index = i;
    sec.name = section_name(*raw, data, size, *header);
    sec.cls = classify(sec.name, raw->characteristics);
    sec.comdat = (raw->characteristics & flag_comdat) != 0;
    // Uninitialized storage: MSVC reports its size in VirtualSize, clang in
    // SizeOfRawData (with no file content behind it), so the flag — not the
    // raw size — decides that no bytes are read, while the size takes the
    // larger of the two fields.
    const bool uninitialized = (raw->characteristics & flag_cnt_uninitialized) != 0;
    sec.size = raw->raw_data_size > raw->virtual_size ? raw->raw_data_size : raw->virtual_size;
    sec.align = std::uint64_t{1} << ((raw->characteristics >> align_shift) & 0xf);
    if (!uninitialized && sec.cls != section_class::other && raw->raw_data_size > 0) {
      const std::uint8_t* start =
          at(data, size, raw->raw_data_offset, raw->raw_data_size, sec.name.c_str());
      sec.bytes.assign(start, start + raw->raw_data_size);
    }
    obj.sections.push_back(std::move(sec));
  }

  return obj;
}

} // namespace neko::pe
