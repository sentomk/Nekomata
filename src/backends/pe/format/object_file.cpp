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

// The natural alignment of this layout would pad to 20 bytes; COFF records
// are exactly 18.
#pragma pack(push, 1)
struct coff_symbol_record {
  char name[8];
  std::uint32_t value;
  std::int16_t section_number;
  std::uint16_t type;
  std::uint8_t storage_class;
  std::uint8_t aux_count;
};
#pragma pack(pop)
static_assert(sizeof(coff_symbol_record) == 18);

constexpr std::uint16_t machine_amd64 = 0x8664;

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
    throw std::runtime_error("name offset is empty");
  }
  for (const char* p = begin; p != end; ++p) {
    if (*p < '0' || *p > '9') {
      throw std::runtime_error("name offset is not decimal");
    }
    value = value * 10 + static_cast<std::uint8_t>(*p - '0');
  }
  return value;
}

// The eight-byte name field of a section or symbol record holds the name
// (NUL-padded, truncation at eight bytes is legal), "/ddd" for sections, or
// — symbol records only — four zero bytes followed by a 32-bit offset; both
// long forms index the string table that follows the symbol table. clang
// names the .llvm_addrsig section the first way; both drivers name mangled
// symbols the second.
std::string field_name(const char (&field)[8], const std::uint8_t* data, std::size_t size,
                       const coff_header& header) {
  std::uint64_t offset = 0;
  bool external = false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(field);
  if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0) {
    if (header.symbol_table_offset == 0) {
      throw std::runtime_error("long name without a symbol table");
    }
    offset = static_cast<std::uint64_t>(bytes[4]) | (static_cast<std::uint64_t>(bytes[5]) << 8) |
             (static_cast<std::uint64_t>(bytes[6]) << 16) |
             (static_cast<std::uint64_t>(bytes[7]) << 24);
    external = true;
  } else if (field[0] == '/') {
    if (header.symbol_table_offset == 0) {
      throw std::runtime_error("long name without a symbol table");
    }
    offset = decimal_offset(field + 1, std::find(field + 1, field + sizeof(field), '\0'));
    external = true;
  }
  if (!external) {
    const char* end = std::find(field, field + sizeof(field), '\0');
    return std::string(field, end);
  }
  const std::uint64_t table =
      static_cast<std::uint64_t>(header.symbol_table_offset) +
      static_cast<std::uint64_t>(header.symbol_count) * sizeof(coff_symbol_record);
  const std::uint8_t* start = at(data, size, table + offset, 1, "name in string table");
  const void* nul = std::memchr(start, 0, size - (static_cast<std::size_t>(start - data)));
  if (nul == nullptr) {
    throw std::runtime_error("truncated object file: name is not terminated");
  }
  return std::string(reinterpret_cast<const char*>(start),
                     static_cast<std::size_t>(static_cast<const char*>(nul) -
                                              reinterpret_cast<const char*>(start)));
}

// The first auxiliary record of a section symbol: the section definition,
// carrying the COMDAT selection at byte 14 and the one-based association at
// byte 12.
constexpr std::size_t aux_association_offset = 12;
constexpr std::size_t aux_selection_offset = 14;

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
    sec.name = field_name(raw->name, data, size, *header);
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

  // The symbol table follows the section table's raw data in the file; an
  // offset of zero means the object has none. Auxiliary records occupy
  // table slots and are kept as placeholders so that a symbol's vector index
  // equals its COFF symbol index, which is what relocations reference.
  if (header->symbol_count > 0) {
    require(header->symbol_table_offset != 0, "symbol table offset missing");
    const std::uint64_t table = static_cast<std::uint64_t>(header->symbol_table_offset);
    at(data, size, table,
       static_cast<std::uint64_t>(header->symbol_count) * sizeof(coff_symbol_record),
       "symbol table");
    obj.symbols.reserve(header->symbol_count);
    for (std::uint32_t i = 0; i < header->symbol_count;) {
      const auto* raw = reinterpret_cast<const coff_symbol_record*>(
          data + table + static_cast<std::uint64_t>(i) * sizeof(coff_symbol_record));
      require(static_cast<std::uint64_t>(i) + 1 + raw->aux_count <= header->symbol_count,
              "auxiliary records run past the symbol table");

      symbol out;
      out.name = field_name(raw->name, data, size, *header);
      out.section_number = raw->section_number;
      out.type = raw->type;
      out.storage_class = raw->storage_class;
      out.value = raw->value;
      if (out.section_number >= 1) {
        require(static_cast<std::uint16_t>(out.section_number) <= header->section_count,
                "symbol section number out of bounds");
      }

      // A static symbol named like its own section is the section symbol;
      // its first auxiliary record carries the COMDAT folding contract.
      if (raw->aux_count > 0 && out.storage_class == 3 && out.section_number >= 1 &&
          out.name == obj.sections[static_cast<std::size_t>(out.section_number) - 1].name) {
        const auto* aux = reinterpret_cast<const std::uint8_t*>(raw) + sizeof(coff_symbol_record);
        const std::uint16_t association =
            static_cast<std::uint16_t>(aux[aux_association_offset]) |
            (static_cast<std::uint16_t>(aux[aux_association_offset + 1]) << 8);
        const std::uint8_t selection = aux[aux_selection_offset];
        require(selection <= 6, "unknown COMDAT selection");
        if (selection == 5) { // associative: the association names its leader
          require(association >= 1 && association <= header->section_count,
                  "COMDAT association out of bounds");
        }
        auto& sec = obj.sections[static_cast<std::size_t>(out.section_number) - 1];
        sec.selection = static_cast<comdat_selection>(selection);
        sec.association = association;
      }

      obj.symbols.push_back(std::move(out));
      ++i;
      for (std::uint16_t a = 0; a < raw->aux_count; ++a, ++i) {
        symbol aux_entry;
        aux_entry.auxiliary = true;
        obj.symbols.push_back(std::move(aux_entry));
      }
    }
  }

  return obj;
}

} // namespace neko::pe
