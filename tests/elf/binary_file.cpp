#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "binary_file.hpp"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t text_offset = 128, strings_offset = 144, sections_offset = 256;
constexpr std::size_t symbols_offset = 512;
constexpr std::uint64_t text_address = 0x401000;

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value,
         std::size_t width) {
  REQUIRE(offset + width <= bytes.size());
  for (std::size_t i = 0; i < width; ++i) {
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

std::vector<std::uint8_t> fixture() {
  std::vector<std::uint8_t> bytes(symbols_offset + 5 * sizeof(Elf64_Sym));
  const std::array<std::uint8_t, 7> ident{0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT};
  std::copy(ident.begin(), ident.end(), bytes.begin());
  put(bytes, 16, ET_EXEC, 2);
  put(bytes, 18, EM_X86_64, 2);
  put(bytes, 20, EV_CURRENT, 4);
  put(bytes, 32, sizeof(Elf64_Ehdr), 8);
  put(bytes, 40, sections_offset, 8);
  put(bytes, 52, sizeof(Elf64_Ehdr), 2);
  put(bytes, 54, sizeof(Elf64_Phdr), 2);
  put(bytes, 56, 1, 2);
  put(bytes, 58, sizeof(Elf64_Shdr), 2);
  put(bytes, 60, 4, 2);
  put(bytes, 64, PT_LOAD, 4);
  put(bytes, 68, PF_R | PF_X, 4);
  put(bytes, 72, text_offset, 8);
  put(bytes, 80, text_address, 8);
  put(bytes, 96, 16, 8);
  put(bytes, 104, 16, 8);
  const auto text = sections_offset + sizeof(Elf64_Shdr);
  put(bytes, text + 4, SHT_PROGBITS, 4);
  put(bytes, text + 8, SHF_ALLOC | SHF_EXECINSTR, 8);
  put(bytes, text + 16, text_address, 8);
  put(bytes, text + 24, text_offset, 8);
  put(bytes, text + 32, 16, 8);
  const auto strings = sections_offset + 2 * sizeof(Elf64_Shdr);
  const char names[] = "\0helper\0alias\0unknown";
  std::copy(std::begin(names), std::end(names), bytes.begin() + strings_offset);
  put(bytes, strings + 4, SHT_STRTAB, 4);
  put(bytes, strings + 24, strings_offset, 8);
  put(bytes, strings + 32, sizeof(names), 8);
  const auto symbols = sections_offset + 3 * sizeof(Elf64_Shdr);
  put(bytes, symbols + 4, SHT_SYMTAB, 4);
  put(bytes, symbols + 24, symbols_offset, 8);
  put(bytes, symbols + 32, 5 * sizeof(Elf64_Sym), 8);
  put(bytes, symbols + 40, 2, 4);
  put(bytes, symbols + 44, 3, 4);
  put(bytes, symbols + 56, sizeof(Elf64_Sym), 8);
  for (std::size_t i = 1; i < 5; ++i) {
    const auto symbol = symbols_offset + i * sizeof(Elf64_Sym);
    put(bytes, symbol, i <= 2 ? 1 : (i == 3 ? 8 : 14), 4);
    put(bytes, symbol + 4, ELF64_ST_INFO(i <= 2 ? STB_LOCAL : STB_WEAK, STT_FUNC), 1);
    put(bytes, symbol + 6, 1, 2);
    put(bytes, symbol + 8, text_address + (i == 2 ? 4 : (i == 4 ? 8 : 0)), 8);
    put(bytes, symbol + 16, i == 4 ? 0 : 4, 8);
  }
  return bytes;
}

class sample {
public:
  sample() {
    auto pattern = (std::filesystem::temp_directory_path() / "nekomata-elf-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("cannot create fixture directory");
    }
    directory_ = pattern;
  }
  ~sample() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  sample(const sample&) = delete;
  sample& operator=(const sample&) = delete;
  std::filesystem::path path() const { return directory_ / "sample with spaces"; }
  void write(const std::vector<std::uint8_t>& bytes) const {
    std::ofstream output(path(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
  }

private:
  std::filesystem::path directory_;
};

TEST_CASE("ELF functions retain duplicate names, aliases and unknown sizes") {
  const sample input;
  input.write(fixture());
  const neko::elf::binary_file file(input.path());
  const auto symbols = file.symbols();
  CHECK(symbols.has_symtab);
  REQUIRE(symbols.functions.size() == 4);
  const auto& first = symbols.functions[0];
  const auto& second = symbols.functions[1];
  CHECK(first.name == "helper");
  CHECK(second.name == first.name);
  CHECK(first.address != second.address);
  CHECK(first.binding == STB_LOCAL);
  CHECK(first.table_section == 3);
  CHECK(first.table_index == 1);
  CHECK(second.table_index == 2);
  CHECK(first.section == 1);
  CHECK(first.size == 4);
  CHECK(symbols.functions[2].name == "alias");
  CHECK(symbols.functions[2].address == first.address);
  CHECK(symbols.functions[2].binding == STB_WEAK);
  CHECK(symbols.functions[3].size == 0);
}

TEST_CASE("missing symtab differs from malformed symtab; dynsym is not a substitute") {
  const sample input;
  auto bytes = fixture();
  SUBCASE("no section headers") {
    put(bytes, 40, 0, 8);
    put(bytes, 60, 0, 2);
  }
  SUBCASE("only dynsym") {
    put(bytes, sections_offset + 3 * sizeof(Elf64_Shdr) + 4, SHT_DYNSYM, 4);
  }
  input.write(bytes);
  const auto symbols = neko::elf::binary_file(input.path()).symbols();
  CHECK_FALSE(symbols.has_symtab);
  CHECK(symbols.functions.empty());
}

TEST_CASE("undefined functions and non-functions do not enter the code index") {
  const sample input;
  auto bytes = fixture();
  put(bytes, symbols_offset + sizeof(Elf64_Sym) + 6, SHN_UNDEF, 2);
  put(bytes, symbols_offset + 2 * sizeof(Elf64_Sym) + 4, ELF64_ST_INFO(STB_LOCAL, STT_OBJECT), 1);
  input.write(bytes);
  CHECK(neko::elf::binary_file(input.path()).symbols().functions.size() == 2);
}

TEST_CASE("malformed symbol table metadata is rejected before dereferencing") {
  const sample input;
  auto bytes = fixture();
  const auto table = sections_offset + 3 * sizeof(Elf64_Shdr);
  SUBCASE("zero entry size") {
    put(bytes, table + 56, 0, 8);
  }
  SUBCASE("partial entry") {
    put(bytes, table + 32, 119, 8);
  }
  SUBCASE("bad string table index") {
    put(bytes, table + 40, 999, 4);
  }
  SUBCASE("wrong string section type") {
    put(bytes, table + 40, 1, 4);
  }
  SUBCASE("bad local boundary") {
    put(bytes, table + 44, 100, 4);
  }
  SUBCASE("section offset overflow") {
    put(bytes, 40, std::numeric_limits<std::uint64_t>::max() - 8, 8);
  }
  SUBCASE("section size overflow") {
    put(bytes, table + 32, std::numeric_limits<std::uint64_t>::max(), 8);
  }
  SUBCASE("extended section count") {
    put(bytes, 60, 0, 2);
  }
  SUBCASE("wrong section entry size") {
    put(bytes, 58, 0, 2);
  }
  input.write(bytes);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()).symbols(), std::runtime_error);
}

TEST_CASE("malformed symbol records and strings fail closed") {
  const sample input;
  auto bytes = fixture();
  const auto symbol = symbols_offset + sizeof(Elf64_Sym);
  SUBCASE("invalid null symbol") {
    bytes[symbols_offset] = 1;
  }
  SUBCASE("out of bounds string") {
    put(bytes, symbol, 999, 4);
  }
  SUBCASE("missing first NUL") {
    bytes[strings_offset] = 'x';
  }
  SUBCASE("missing final NUL") {
    bytes[strings_offset + 21] = 'x';
  }
  SUBCASE("invalid section index") {
    put(bytes, symbol + 6, 99, 2);
  }
  SUBCASE("extended section index") {
    put(bytes, symbol + 6, SHN_XINDEX, 2);
  }
  SUBCASE("absolute function") {
    put(bytes, symbol + 6, SHN_ABS, 2);
  }
  SUBCASE("non executable section") {
    put(bytes, symbol + 6, 2, 2);
  }
  SUBCASE("out of bounds function") {
    put(bytes, symbol + 8, text_address - 1, 8);
  }
  SUBCASE("function at exclusive section end") {
    put(bytes, symbol + 8, text_address + 16, 8);
  }
  SUBCASE("size overflow") {
    put(bytes, symbol + 16, std::numeric_limits<std::uint64_t>::max(), 8);
  }
  input.write(bytes);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()).symbols(), std::runtime_error);
}

TEST_CASE("reads stay on the opened file after pathname replacement") {
  const sample input;
  input.write(fixture());
  const neko::elf::binary_file file(input.path());
  std::filesystem::rename(input.path(), input.path().string() + ".old");
  input.write({});
  CHECK(file.symbols().functions.size() == 4);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()), std::runtime_error);
}

TEST_CASE("invalid headers and non-regular paths are rejected") {
  const sample input;
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()), std::runtime_error);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path().parent_path()), std::runtime_error);
  auto bytes = fixture();
  SUBCASE("truncated header") {
    bytes.resize(10);
  }
  SUBCASE("PIE") {
    put(bytes, 16, ET_DYN, 2);
  }
  SUBCASE("relocatable") {
    put(bytes, 16, ET_REL, 2);
  }
  SUBCASE("wrong architecture") {
    put(bytes, 18, EM_AARCH64, 2);
  }
  SUBCASE("segment offset overflow") {
    put(bytes, 32, std::numeric_limits<std::uint64_t>::max(), 8);
  }
  SUBCASE("segment address overflow") {
    put(bytes, 80, std::numeric_limits<std::uint64_t>::max() - 1, 8);
  }
  input.write(bytes);
  CHECK_THROWS_AS(neko::elf::binary_file(input.path()), std::runtime_error);
}

} // namespace
