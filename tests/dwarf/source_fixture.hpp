#pragma once

// Small, explicit DWARF 4 records inside a non-executed ELF image. These
// fixtures cover metadata compilers normally do not emit (missing/corrupt
// coordinates and cross-CU references) without depending on a producer.
#include <dwarf.h>
#include <elf.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace source_fixture {

struct bytes : std::vector<std::uint8_t> {
  std::uint64_t at_number(std::size_t offset, unsigned width) const {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
      value |= static_cast<std::uint64_t>(at(offset + i)) << (i * 8);
    }
    return value;
  }
  void number(std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
  }
  void patch(std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
    }
  }
  void leb(std::uint64_t value) {
    do {
      const auto part = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      push_back(part | (value ? 0x80 : 0));
    } while (value);
  }
  void text(const std::string& value) {
    insert(end(), value.begin(), value.end());
    push_back(0);
  }
  void append(const bytes& value) { insert(end(), value.begin(), value.end()); }
};

struct attribute {
  std::uint64_t kind;
  std::uint64_t form;
  std::uint64_t value = 0;
  std::string text;
};

inline attribute number(std::uint64_t kind, std::uint64_t value,
                        std::uint64_t form = DW_FORM_data4) {
  return {kind, form, value, {}};
}
inline attribute text(std::uint64_t kind, std::string value) {
  return {kind, DW_FORM_string, 0, std::move(value)};
}
// Reference values are node indexes across all units, resolved after encoding.
inline attribute reference(std::uint64_t kind, std::uint64_t node) {
  return number(kind, node, DW_FORM_ref_addr);
}

using node = std::vector<attribute>;
inline node function() {
  return {text(DW_AT_name, "tick"),    number(DW_AT_low_pc, 0x401000, DW_FORM_addr),
          number(DW_AT_high_pc, 4),    number(DW_AT_decl_file, 1),
          number(DW_AT_decl_line, 12), number(DW_AT_decl_column, 4)};
}

struct file_entry {
  std::string name = "unit.cpp";
  std::uint64_t directory = 0;
};
struct unit {
  std::string directory = "/recorded/build";
  std::vector<std::string> includes;
  std::vector<file_entry> files{file_entry{}};
  std::vector<node> nodes{function()};
  bool has_lines = true;
  std::uint16_t line_version = 4;
  bytes line_program{};
  std::optional<std::uint64_t> statement_offset{}; // Reuse an earlier CU's line table.
};

inline bytes line_table(const unit& input) {
  bytes out;
  out.number(0, 4);
  out.number(input.line_version, 2);
  out.number(0, 4);
  for (const auto value : {1, 1, 1, 251, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1}) {
    out.number(static_cast<std::uint64_t>(value), 1);
  }
  for (const auto& include : input.includes) {
    out.text(include);
  }
  out.number(0, 1);
  for (const auto& file : input.files) {
    out.text(file.name);
    out.leb(file.directory);
    out.leb(0);
    out.leb(0);
  }
  out.number(0, 1);
  out.patch(6, out.size() - 10, 4);
  out.append(input.line_program);
  out.number(0, 1); // DW_LNE_end_sequence
  out.leb(1);
  out.number(DW_LNE_end_sequence, 1);
  out.patch(0, out.size() - 4, 4);
  return out;
}

inline bytes elf(const bytes& info, const bytes& abbrev, const bytes& lines,
                 const bytes& ranges = {}) {
  bytes out;
  out.resize(128 + 16); // ELF header, one executable segment, then dummy code.
  out[0] = 0x7f;
  out[1] = 'E';
  out[2] = 'L';
  out[3] = 'F';
  out[EI_CLASS] = ELFCLASS64;
  out[EI_DATA] = ELFDATA2LSB;
  out[EI_VERSION] = EV_CURRENT;
  out.patch(16, ET_EXEC, 2);
  out.patch(18, EM_X86_64, 2);
  out.patch(20, EV_CURRENT, 4);
  out.patch(32, 64, 8);
  out.patch(52, 64, 2);
  out.patch(54, 56, 2);
  out.patch(56, 1, 2);
  out.patch(58, 64, 2);
  const auto has_ranges = !ranges.empty();
  out.patch(60, has_ranges ? 7 : 6, 2);
  out.patch(62, has_ranges ? 6 : 5, 2);
  out.patch(64, PT_LOAD, 4);
  out.patch(68, PF_R | PF_X, 4);
  out.patch(72, 128, 8);
  out.patch(80, 0x401000, 8);
  out.patch(96, 16, 8);
  out.patch(104, 16, 8);
  const auto info_offset = out.size();
  out.append(info);
  const auto abbrev_offset = out.size();
  out.append(abbrev);
  const auto lines_offset = out.size();
  out.append(lines);
  const auto ranges_offset = out.size();
  out.append(ranges);
  const auto names_offset = out.size();
  out.number(0, 1);
  out.text(".text");
  out.text(".debug_info");
  out.text(".debug_abbrev");
  out.text(".debug_line");
  out.text(".debug_ranges");
  out.text(".shstrtab");
  const auto names_size = out.size() - names_offset;
  const auto sections = out.size();
  out.resize(out.size() + (has_ranges ? 7 : 6) * 64);
  out.patch(40, sections, 8);
  const auto section = [&](unsigned index, unsigned name, unsigned type, std::uint64_t offset,
                           std::uint64_t size) {
    const auto base = sections + index * 64;
    out.patch(base, name, 4);
    out.patch(base + 4, type, 4);
    out.patch(base + 24, offset, 8);
    out.patch(base + 32, size, 8);
    out.patch(base + 48, 1, 8);
  };
  section(1, 1, SHT_PROGBITS, 128, 16);
  out.patch(sections + 64 + 8, SHF_ALLOC | SHF_EXECINSTR, 8);
  out.patch(sections + 64 + 16, 0x401000, 8);
  section(2, 7, SHT_PROGBITS, info_offset, info.size());
  section(3, 19, SHT_PROGBITS, abbrev_offset, abbrev.size());
  section(4, 33, SHT_PROGBITS, lines_offset, lines.size());
  if (has_ranges) {
    section(5, 45, SHT_PROGBITS, ranges_offset, ranges.size());
    section(6, 59, SHT_STRTAB, names_offset, names_size);
  } else {
    section(5, 59, SHT_STRTAB, names_offset, names_size);
  }
  return out;
}

inline bytes build(const std::vector<unit>& units) {
  bytes info, abbrev, lines;
  std::vector<std::size_t> nodes;
  std::vector<std::pair<std::size_t, std::uint64_t>> references;
  std::uint64_t code = 0;
  const auto emit = [&](const node& attributes, std::uint64_t tag, bool children) {
    abbrev.leb(++code);
    abbrev.leb(tag);
    abbrev.number(children ? 1 : 0, 1);
    info.leb(code);
    for (const auto& attr : attributes) {
      abbrev.leb(attr.kind);
      abbrev.leb(attr.form);
      switch (attr.form) {
      case DW_FORM_string:
        info.text(attr.text);
        break;
      case DW_FORM_addr:
      case DW_FORM_data8:
        info.number(attr.value, 8);
        break;
      case DW_FORM_ref_addr:
        references.emplace_back(info.size(), attr.value);
        info.number(0, 4);
        break;
      case DW_FORM_flag_present:
        break;
      case DW_FORM_sdata:
        // The signed test encodings are only 0..63 or -1 (UINT64_MAX).
        info.number(attr.value & 0x7f, 1);
        break;
      default:
        info.number(attr.value, 4);
        break;
      }
    }
    abbrev.number(0, 2);
  };
  for (const auto& unit : units) {
    const auto begin = info.size();
    info.number(0, 4);
    info.number(4, 2);
    info.number(0, 4);
    info.number(8, 1);
    node attrs{text(DW_AT_name, "unit.cpp"), text(DW_AT_comp_dir, unit.directory)};
    if (unit.has_lines) {
      attrs.push_back(number(DW_AT_stmt_list, unit.statement_offset.value_or(lines.size()),
                             DW_FORM_sec_offset));
      if (!unit.statement_offset) {
        lines.append(line_table(unit));
      }
    }
    emit(attrs, DW_TAG_compile_unit, true);
    for (const auto& entry : unit.nodes) {
      nodes.push_back(info.size());
      emit(entry, DW_TAG_subprogram, false);
    }
    info.number(0, 1);
    info.patch(begin, info.size() - begin - 4, 4);
  }
  abbrev.number(0, 1);
  for (const auto& [offset, node_index] : references) {
    info.patch(offset, node_index < nodes.size() ? nodes[node_index] : 0xfffffff0, 4);
  }
  return elf(info, abbrev, lines);
}

inline bytes section_data(const bytes& image, unsigned index) {
  const auto entry = image.at_number(40, 8) + index * 64;
  const auto begin = static_cast<std::size_t>(image.at_number(entry + 24, 8));
  const auto size = static_cast<std::size_t>(image.at_number(entry + 32, 8));
  bytes result;
  for (std::size_t i = 0; i < size; ++i) {
    result.push_back(image.at(begin + i));
  }
  return result;
}

class sample {
public:
  explicit sample(const bytes& content) {
    auto pattern = (std::filesystem::temp_directory_path() / "neko-source-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("cannot create source fixture directory");
    }
    directory_ = pattern;
    std::ofstream output(path(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    if (!output) {
      throw std::runtime_error("cannot write source fixture");
    }
  }
  ~sample() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  sample(const sample&) = delete;
  sample& operator=(const sample&) = delete;
  std::filesystem::path path() const { return directory_ / "source fixture.elf"; }

private:
  std::filesystem::path directory_;
};

} // namespace source_fixture
