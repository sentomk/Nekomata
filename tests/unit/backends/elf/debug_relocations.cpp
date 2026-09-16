#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "binary_file.hpp"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

std::array<std::filesystem::path, 2> compiler_objects;

void put(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) {
    bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
  }
}

struct fixture {
  static constexpr std::size_t info = 1, first_code = 2, second_code = 3, debug_strings = 4;
  static constexpr std::size_t symtab = 5, strtab = 6, rela = 7, shstrtab = 8;
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(64 + 9 * 64);
  std::vector<std::uint8_t> names{0};
  std::size_t symbols = 0, relocations = 0, strings = 0;

  static std::size_t header(std::size_t index) { return 64 + index * 64; }
  std::size_t section(std::size_t index, const std::string& name, unsigned type, std::size_t size,
                      std::uint64_t flags = 0) {
    put(bytes, header(index), names.size(), 4);
    names.insert(names.end(), name.begin(), name.end());
    names.push_back(0);
    put(bytes, header(index) + 4, type, 4);
    put(bytes, header(index) + 8, flags, 8);
    put(bytes, header(index) + 24, bytes.size(), 8);
    put(bytes, header(index) + 32, size, 8);
    const auto offset = bytes.size();
    bytes.resize(offset + size);
    return offset;
  }
  fixture() {
    const std::array<std::uint8_t, 7> ident{0x7f,       'E',         'L',       'F',
                                            ELFCLASS64, ELFDATA2LSB, EV_CURRENT};
    std::copy(ident.begin(), ident.end(), bytes.begin());
    put(bytes, 16, ET_REL, 2);
    put(bytes, 18, EM_X86_64, 2);
    put(bytes, 20, EV_CURRENT, 4);
    put(bytes, 40, 64, 8);
    put(bytes, 52, 64, 2);
    put(bytes, 58, 64, 2);
    put(bytes, 60, 9, 2);
    put(bytes, 62, shstrtab, 2);
    section(info, ".debug_info", SHT_PROGBITS, 32);
    section(first_code, ".text.first", SHT_PROGBITS, 16, SHF_ALLOC | SHF_EXECINSTR);
    section(second_code, ".text.second", SHT_PROGBITS, 16, SHF_ALLOC | SHF_EXECINSTR);
    section(debug_strings, ".debug_str", SHT_PROGBITS, 16);
    symbols = section(symtab, ".symtab", SHT_SYMTAB, 4 * sizeof(Elf64_Sym));
    put(bytes, header(symtab) + 40, strtab, 4);
    put(bytes, header(symtab) + 44, 4, 4);
    put(bytes, header(symtab) + 56, sizeof(Elf64_Sym), 8);
    for (std::size_t i = 1; i < 4; ++i) {
      put(bytes, symbols + i * sizeof(Elf64_Sym) + 4, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), 1);
      put(bytes, symbols + i * sizeof(Elf64_Sym) + 6, i + 1, 2);
    }
    strings = section(strtab, ".strtab", SHT_STRTAB, 1);
    // Deliberately not named .rela.debug_info: sh_info, not the name, is authoritative.
    relocations = section(rela, ".renamed_relocations", SHT_RELA, 3 * sizeof(Elf64_Rela));
    put(bytes, header(rela) + 40, symtab, 4);
    put(bytes, header(rela) + 44, info, 4);
    put(bytes, header(rela) + 56, sizeof(Elf64_Rela), 8);
    for (std::size_t i = 0; i < 3; ++i) {
      put(bytes, relocations + i * sizeof(Elf64_Rela), i * 8, 8);
      put(bytes, relocations + i * sizeof(Elf64_Rela) + 8,
          ELF64_R_INFO(i + 1, i == 2 ? R_X86_64_32 : R_X86_64_64), 8);
    }
    put(bytes, relocations + 2 * sizeof(Elf64_Rela) + 16, 3, 8);
    const auto offset = section(shstrtab, ".shstrtab", SHT_STRTAB, 0);
    bytes.insert(bytes.end(), names.begin(), names.end());
    put(bytes, header(shstrtab) + 32, bytes.size() - offset, 8);
  }
};

class sample {
public:
  explicit sample(const std::vector<std::uint8_t>& bytes) {
    auto pattern = (std::filesystem::temp_directory_path() / "neko-relocations-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("cannot create relocation fixture directory");
    }
    directory_ = pattern;
    write(bytes);
  }
  ~sample() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  sample(const sample&) = delete;
  sample& operator=(const sample&) = delete;
  std::filesystem::path path() const { return directory_ / "object with spaces.o"; }
  void write(const std::vector<std::uint8_t>& bytes) const {
    std::ofstream output(path(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("cannot write relocation fixture");
    }
  }

private:
  std::filesystem::path directory_;
};

// Add a second symbol table and a second relocation table. Existing section
// indexes remain unchanged; the original three relocations are split 2 + 1.
std::size_t split_tables(fixture& data) {
  const auto base = data.bytes.size();
  data.bytes.resize(base + 11 * 64);
  std::copy_n(data.bytes.begin() + 64, 9 * 64,
              data.bytes.begin() + static_cast<std::ptrdiff_t>(base));
  for (const auto& [source, target] :
       {std::pair{fixture::symtab, std::size_t{9}}, std::pair{fixture::rela, std::size_t{10}}}) {
    std::copy_n(data.bytes.begin() + static_cast<std::ptrdiff_t>(fixture::header(source)), 64,
                data.bytes.begin() + static_cast<std::ptrdiff_t>(base + target * 64));
  }
  put(data.bytes, 40, base, 8);
  put(data.bytes, 60, 11, 2);
  put(data.bytes, base + fixture::rela * 64 + 32, 2 * sizeof(Elf64_Rela), 8);
  put(data.bytes, base + 10 * 64 + 24, data.relocations + 2 * sizeof(Elf64_Rela), 8);
  put(data.bytes, base + 10 * 64 + 32, sizeof(Elf64_Rela), 8);
  return base;
}

TEST_CASE("debug relocations retain field, symbol and section identities without writing") {
  const fixture data;
  const sample input(data.bytes);
  const neko::elf::binary_file file(input.path());
  const auto result = file.debug_info_relocations();
  CHECK(result.info_section == fixture::info);
  REQUIRE(result.entries.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto& value = result.entries[i];
    CHECK(value.relocation_section == fixture::rela);
    CHECK(value.relocation_index == i);
    CHECK(value.offset == i * 8);
    CHECK(value.symbol_table == fixture::symtab);
    CHECK(value.symbol_index == i + 1);
    CHECK(value.symbol_section == i + 2);
    CHECK(value.symbol_value == 0);
    CHECK(value.addend == (i == 2 ? 3 : 0));
    CHECK(value.resolved_offset == (i == 2 ? 3 : 0));
    CHECK(value.width == (i == 2 ? 4 : 8));
    CHECK(value.type == (i == 2 ? R_X86_64_32 : R_X86_64_64));
  }
  std::vector<std::uint8_t> after(data.bytes.size());
  file.read(0, after);
  CHECK(after == data.bytes);
  std::filesystem::rename(input.path(), input.path().string() + ".old");
  input.write({});
  CHECK(file.debug_info_relocations().entries.size() == 3);
}

TEST_CASE("missing debug information differs from a present section without relocations") {
  fixture data;
  bool missing = false;
  SUBCASE("no debug info") {
    put(data.bytes, fixture::header(fixture::info), 0, 4);
    missing = true;
  }
  SUBCASE("unrelated relocation section is not consumed") {
    put(data.bytes, fixture::header(fixture::rela) + 44, fixture::first_code, 4);
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(1, R_X86_64_PC32), 8);
  }
  SUBCASE("empty relocation table") {
    put(data.bytes, fixture::header(fixture::rela) + 32, 0, 8);
  }
  const sample input(data.bytes);
  const auto result = neko::elf::binary_file(input.path()).debug_info_relocations();
  CHECK(result.info_section.has_value() == !missing);
  CHECK(result.entries.empty());
}

TEST_CASE("signed addends, exclusive endpoints and input ordering are explicit") {
  fixture data;
  std::uint64_t expected = 3;
  SUBCASE("negative addend") {
    put(data.bytes, data.symbols + sizeof(Elf64_Sym) + 8, 5, 8);
    put(data.bytes, data.relocations + 16, std::numeric_limits<std::uint64_t>::max() - 1, 8);
  }
  SUBCASE("exclusive section end") {
    expected = 16;
    put(data.bytes, data.relocations + 16, 16, 8);
  }
  SUBCASE("INT64_MIN without signed overflow") {
    expected = 0;
    put(data.bytes, fixture::header(fixture::first_code) + 4, SHT_NOBITS, 4);
    put(data.bytes, fixture::header(fixture::first_code) + 32, std::uint64_t{1} << 63, 8);
    put(data.bytes, data.symbols + sizeof(Elf64_Sym) + 8, std::uint64_t{1} << 63, 8);
    put(data.bytes, data.relocations + 16, std::uint64_t{1} << 63, 8);
  }
  SUBCASE("out of order fields") {
    expected = 0;
    put(data.bytes, data.relocations, 8, 8);
    put(data.bytes, data.relocations + sizeof(Elf64_Rela), 0, 8);
  }
  const sample input(data.bytes);
  const auto result = neko::elf::binary_file(input.path()).debug_info_relocations();
  REQUIRE(result.entries.size() == 3);
  CHECK(result.entries.front().offset == 0);
  CHECK(result.entries.front().resolved_offset == expected);
}

TEST_CASE("invalid debug relocation metadata and arithmetic fail closed") {
  fixture data;
  const auto rel = fixture::header(fixture::rela);
  const auto sym = data.symbols + sizeof(Elf64_Sym);
  const auto code = fixture::header(fixture::first_code);
  SUBCASE("missing names") {
    put(data.bytes, 62, 0, 2);
  }
  SUBCASE("invalid name index") {
    put(data.bytes, fixture::header(1), 9999, 4);
  }
  SUBCASE("bad name terminator") {
    data.bytes.back() = 'x';
  }
  SUBCASE("duplicate debug info") {
    put(data.bytes, code, 1, 4);
  }
  SUBCASE("compressed debug info") {
    put(data.bytes, fixture::header(1) + 8, SHF_COMPRESSED, 8);
  }
  SUBCASE("grouped debug info") {
    put(data.bytes, fixture::header(1) + 8, SHF_GROUP, 8);
  }
  SUBCASE("REL not RELA") {
    put(data.bytes, rel + 4, SHT_REL, 4);
  }
  SUBCASE("invalid target section") {
    put(data.bytes, rel + 44, 999, 4);
  }
  SUBCASE("invalid symbol table") {
    put(data.bytes, rel + 40, 999, 4);
  }
  SUBCASE("wrong symbol table type") {
    put(data.bytes, rel + 40, fixture::first_code, 4);
  }
  SUBCASE("partial relocation") {
    put(data.bytes, rel + 32, 71, 8);
  }
  SUBCASE("bad entry size") {
    put(data.bytes, rel + 56, 0, 8);
  }
  SUBCASE("truncated relocation data") {
    put(data.bytes, rel + 24, data.bytes.size(), 8);
  }
  SUBCASE("invalid null symbol") {
    data.bytes[data.symbols] = 1;
  }
  SUBCASE("null reference") {
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(0, R_X86_64_64), 8);
  }
  SUBCASE("bad symbol index") {
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(99, R_X86_64_64), 8);
  }
  SUBCASE("bad symbol name") {
    put(data.bytes, sym, 99, 4);
  }
  SUBCASE("bad string table") {
    data.bytes[data.strings] = 1;
  }
  SUBCASE("undefined symbol") {
    put(data.bytes, sym + 6, SHN_UNDEF, 2);
  }
  SUBCASE("absolute symbol") {
    put(data.bytes, sym + 6, SHN_ABS, 2);
  }
  SUBCASE("extended symbol section") {
    put(data.bytes, sym + 6, SHN_XINDEX, 2);
  }
  SUBCASE("out of range symbol section") {
    put(data.bytes, sym + 6, 999, 2);
  }
  SUBCASE("symbol outside section") {
    put(data.bytes, sym + 8, 17, 8);
  }
  SUBCASE("symbol size outside section") {
    put(data.bytes, sym + 16, 17, 8);
  }
  SUBCASE("unsupported PC relative type") {
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(1, R_X86_64_PC32), 8);
  }
  SUBCASE("unsupported NONE") {
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(1, R_X86_64_NONE), 8);
  }
  SUBCASE("unsupported signed 32") {
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(1, R_X86_64_32S), 8);
  }
  SUBCASE("field crosses end") {
    put(data.bytes, data.relocations, 28, 8);
  }
  SUBCASE("field overflow") {
    put(data.bytes, data.relocations, std::numeric_limits<std::uint64_t>::max(), 8);
  }
  SUBCASE("negative underflow") {
    put(data.bytes, data.relocations + 16, std::uint64_t{1} << 63, 8);
  }
  SUBCASE("resolved offset outside section") {
    put(data.bytes, data.relocations + 16, 17, 8);
  }
  SUBCASE("64 bit addition overflow") {
    put(data.bytes, code + 4, SHT_NOBITS, 4);
    put(data.bytes, code + 32, std::numeric_limits<std::uint64_t>::max(), 8);
    put(data.bytes, sym + 8, std::numeric_limits<std::uint64_t>::max(), 8);
    put(data.bytes, data.relocations + 16, 1, 8);
  }
  SUBCASE("32 bit truncation") {
    put(data.bytes, code + 4, SHT_NOBITS, 4);
    put(data.bytes, code + 32, std::uint64_t{1} << 32, 8);
    put(data.bytes, data.relocations + 8, ELF64_R_INFO(1, R_X86_64_32), 8);
    put(data.bytes, data.relocations + 16, std::uint64_t{1} << 32, 8);
  }
  SUBCASE("duplicate fields") {
    put(data.bytes, data.relocations + sizeof(Elf64_Rela), 0, 8);
  }
  SUBCASE("partially overlapping fields") {
    put(data.bytes, data.relocations + sizeof(Elf64_Rela), 4, 8);
  }
  const sample input(data.bytes);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()).debug_info_relocations(),
                  std::runtime_error);
}

TEST_CASE("oversized relocation tables are rejected before allocating their result") {
  fixture data;
  const auto offset = data.bytes.size();
  constexpr std::size_t size = 1000001 * sizeof(Elf64_Rela);
  data.bytes.resize(offset + size);
  put(data.bytes, fixture::header(fixture::rela) + 24, offset, 8);
  put(data.bytes, fixture::header(fixture::rela) + 32, size, 8);
  SUBCASE("one oversized table") {}
  SUBCASE("individually bounded tables exceed the total budget") {
    const auto base = split_tables(data);
    put(data.bytes, base + fixture::rela * 64 + 24, data.relocations, 8);
    put(data.bytes, base + 10 * 64 + 24, offset, 8);
    put(data.bytes, base + 10 * 64 + 32, 999999 * sizeof(Elf64_Rela), 8);
  }
  const sample input(data.bytes);
  CHECK_THROWS_WITH(neko::elf::binary_file(input.path()).debug_info_relocations(),
                    doctest::Contains("debug relocations exceed inspection limits"));
}

TEST_CASE("multiple relocation tables retain identity and cannot conceal overlapping fields") {
  fixture data;
  const auto base = split_tables(data);
  bool overlap = false;
  SUBCASE("disjoint fields through another symbol table") {
    put(data.bytes, base + 10 * 64 + 40, 9, 4);
  }
  SUBCASE("overlap across tables") {
    put(data.bytes, data.relocations + 2 * sizeof(Elf64_Rela), 4, 8);
    overlap = true;
  }
  const sample input(data.bytes);
  const neko::elf::binary_file file(input.path());
  if (overlap) {
    CHECK_THROWS_WITH(file.debug_info_relocations(), doctest::Contains("overlapping"));
  } else {
    const auto result = file.debug_info_relocations();
    REQUIRE(result.entries.size() == 3);
    CHECK(result.entries.back().relocation_section == 10);
    CHECK(result.entries.back().relocation_index == 0);
    CHECK(result.entries.back().symbol_table == 9);
    CHECK(result.entries.back().symbol_index == 3);
    CHECK(result.entries.back().resolved_offset == 3);
  }
}

TEST_CASE("symbol budgets count each referenced table once and apply across tables") {
  fixture data;
  constexpr std::size_t count = 500001;
  const auto offset = data.bytes.size();
  data.bytes.resize(offset + count * sizeof(Elf64_Sym));
  std::copy_n(data.bytes.begin() + static_cast<std::ptrdiff_t>(data.symbols), 4 * sizeof(Elf64_Sym),
              data.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  put(data.bytes, fixture::header(fixture::symtab) + 24, offset, 8);
  put(data.bytes, fixture::header(fixture::symtab) + 32, count * sizeof(Elf64_Sym), 8);
  const auto base = split_tables(data);
  bool shared = true;
  SUBCASE("shared symbol table is read once") {}
  SUBCASE("distinct table identities share a file-wide budget") {
    put(data.bytes, base + 10 * 64 + 40, 9, 4);
    shared = false;
  }
  const sample input(data.bytes);
  const neko::elf::binary_file file(input.path());
  if (shared) {
    CHECK(file.debug_info_relocations().entries.size() == 3);
  } else {
    CHECK_THROWS_WITH(file.debug_info_relocations(), doctest::Contains("symbol tables exceed"));
  }
}

TEST_CASE("string table reads have a file-wide byte budget") {
  fixture data;
  constexpr std::size_t size = 32 * 1024 * 1024;
  const auto offset = data.bytes.size();
  data.bytes.resize(offset + size);
  put(data.bytes, fixture::header(fixture::strtab) + 24, offset, 8);
  put(data.bytes, fixture::header(fixture::strtab) + 32, size, 8);
  const auto base = split_tables(data);
  put(data.bytes, base + 10 * 64 + 40, 9, 4);
  const sample input(data.bytes);
  CHECK_THROWS_WITH(neko::elf::binary_file(input.path()).debug_info_relocations(),
                    doctest::Contains("oversized relocation string table"));
}

TEST_CASE("compiler-produced DWARF references preserve distinct zero-offset code sections") {
  for (const auto& path : compiler_objects) {
    CAPTURE(path);
    const neko::elf::binary_file file(path);
    const auto before = file.symbols();
    const auto result = file.debug_info_relocations();
    REQUIRE(result.info_section);
    REQUIRE_FALSE(result.entries.empty());
    std::set<std::uint32_t> code_sections;
    bool has32 = false, has64 = false;
    for (const auto& value : result.entries) {
      has32 = has32 || value.type == R_X86_64_32;
      has64 = has64 || value.type == R_X86_64_64;
      for (const auto& symbol : before.functions) {
        if (symbol.section == value.symbol_section && value.resolved_offset == 0) {
          code_sections.insert(symbol.section);
        }
      }
    }
    CHECK(has32);
    CHECK(has64);
    CHECK(code_sections.size() == 2);
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  std::copy_n(argv + 1, compiler_objects.size(), compiler_objects.begin());
  doctest::Context context;
  return context.run();
}
