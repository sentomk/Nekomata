#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "function_index.hpp"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

std::filesystem::path fixture_path;
std::filesystem::path aliases_path;

neko::dwarf::binary_info debug_sample() {
  neko::dwarf::binary_info debug;
  neko::dwarf::compilation_unit unit;
  unit.name = "a.cpp";
  unit.functions.push_back({1, "helper", "_ZL6helperv", {{0x1000, 0x1010}}});
  debug.units.push_back(std::move(unit));
  return debug;
}

neko::elf::function_symbols symbol_sample() {
  return {true, {{3, 1, 1, "_ZL6helperv", 0x1000, 16, 0, 0}}};
}

TEST_CASE("association retains distinct same-name local definitions and symbol identities") {
  auto debug = debug_sample();
  auto second = debug.units.front();
  second.name = "b.cpp";
  second.functions.front().die_offset = 2;
  second.functions.front().ranges = {{0x2000, 0x2010}};
  debug.units.push_back(second);
  auto symbols = symbol_sample();
  auto other = symbols.functions.front();
  other.address = 0x2000;
  other.table_index = 2;
  symbols.functions.push_back(other);
  // Input order need not match addresses, source names or DIE ordering.
  std::reverse(symbols.functions.begin(), symbols.functions.end());
  const auto index = neko::elf::associate(debug, symbols);
  REQUIRE(index.functions.size() == 2);
  CHECK(index.functions[0].status == neko::elf::match_status::matched);
  CHECK(index.functions[1].status == neko::elf::match_status::matched);
  CHECK(index.functions[0].candidates == std::vector<std::size_t>{1});
  CHECK(index.functions[1].candidates == std::vector<std::size_t>{0});
  CHECK(index.functions[0].unit == 0);
  CHECK(index.functions[1].unit == 1);
  CHECK(index.unassociated_symbols.empty());
}

TEST_CASE("aliases stay ambiguous even when one linkage name matches") {
  auto symbols = symbol_sample();
  auto alias = symbols.functions.front();
  alias.name = "alias";
  alias.binding = 2;
  alias.table_index = 2;
  symbols.functions.push_back(alias);
  const auto index = neko::elf::associate(debug_sample(), symbols);
  REQUIRE(index.functions.size() == 1);
  CHECK(index.functions[0].status == neko::elf::match_status::ambiguous);
  CHECK(index.functions[0].candidates == std::vector<std::size_t>{0, 1});
  CHECK(index.unassociated_symbols.empty());
}

TEST_CASE("multiple DWARF definitions sharing one ELF entry remain ambiguous") {
  auto debug = debug_sample();
  debug.units.push_back(debug.units.front());
  const auto index = neko::elf::associate(debug, symbol_sample());
  REQUIRE(index.functions.size() == 2);
  for (const auto& match : index.functions) {
    CHECK(match.status == neko::elf::match_status::ambiguous);
  }
}

TEST_CASE("candidate fanout is bounded before constructing a quadratic result") {
  auto debug = debug_sample();
  const auto function = debug.units.front().functions.front();
  debug.units.front().functions.resize(1001, function);
  auto symbols = symbol_sample();
  const auto symbol = symbols.functions.front();
  symbols.functions.resize(1000, symbol);
  CHECK_THROWS_WITH(neko::elf::associate(debug, symbols),
                    doctest::Contains("association exceeds candidate limit"));
}

TEST_CASE("missing and conflicting evidence is never replaced with a name guess") {
  auto debug = debug_sample();
  auto symbols = symbol_sample();
  auto expected = neko::elf::match_status::missing_symbol;
  SUBCASE("missing symtab") {
    symbols = {};
    expected = neko::elf::match_status::missing_symtab;
  }
  SUBCASE("empty symtab") {
    symbols.functions.clear();
  }
  SUBCASE("same name at another address") {
    symbols.functions.front().address = 0x2000;
  }
  SUBCASE("symbol containing the DWARF entry does not establish an exact entry") {
    symbols.functions.front().address = 0x0fff;
    symbols.functions.front().size = 17;
  }
  SUBCASE("unknown size") {
    symbols.functions.front().size = 0;
    expected = neko::elf::match_status::unknown_size;
  }
  SUBCASE("different size") {
    symbols.functions.front().size = 17;
    expected = neko::elf::match_status::range_mismatch;
  }
  SUBCASE("size must not wrap") {
    symbols.functions.front().size = std::numeric_limits<std::uint64_t>::max();
    expected = neko::elf::match_status::range_mismatch;
  }
  SUBCASE("linkage name conflict") {
    symbols.functions.front().name = "not_helper";
    expected = neko::elf::match_status::linkage_mismatch;
  }
  SUBCASE("no emitted range") {
    debug.units.front().functions.front().ranges.clear();
    expected = neko::elf::match_status::unsupported_ranges;
  }
  SUBCASE("multiple ranges") {
    debug.units.front().functions.front().ranges.push_back({0x2000, 0x2010});
    expected = neko::elf::match_status::unsupported_ranges;
  }
  SUBCASE("backward range") {
    debug.units.front().functions.front().ranges.front().end = 0;
    expected = neko::elf::match_status::unsupported_ranges;
  }
  const auto index = neko::elf::associate(debug, symbols);
  REQUIRE(index.functions.size() == 1);
  CHECK(index.functions.front().status == expected);
  CHECK(std::string(neko::elf::status_name(expected)) != "invalid-status");
}

TEST_CASE("missing linkage name is preserved; exact range is separate evidence") {
  auto debug = debug_sample();
  debug.units.front().functions.front().linkage_name.reset();
  const auto index = neko::elf::associate(debug, symbol_sample());
  CHECK(index.functions.front().status == neko::elf::match_status::matched);
  CHECK_FALSE(index.debug.units.front().functions.front().linkage_name);
}

TEST_CASE("ELF code without DWARF is retained separately") {
  auto symbols = symbol_sample();
  auto startup = symbols.functions.front();
  startup.name = "_start";
  startup.address = 0x5000;
  symbols.functions.push_back(startup);
  const auto index = neko::elf::associate(debug_sample(), symbols);
  CHECK(index.unassociated_symbols == std::vector<std::size_t>{1});
}

TEST_CASE("real multi-TU fixture joins every emitted DWARF function") {
  const auto index = neko::elf::inspect_functions(fixture_path);
  REQUIRE(index.functions.size() == 10);
  std::set<std::uint64_t> helpers;
  std::set<std::uint64_t> hidden;
  std::set<std::string> overloads;
  for (const auto& match : index.functions) {
    CHECK(match.status == neko::elf::match_status::matched);
    REQUIRE(match.candidates.size() == 1);
    const auto& function = index.debug.units[match.unit].functions[match.function];
    const auto& symbol = index.symbols.functions[match.candidates.front()];
    REQUIRE(function.ranges.size() == 1);
    CHECK(symbol.address == function.ranges.front().begin);
    CHECK(symbol.size == function.ranges.front().end - symbol.address);
    if (function.name == "helper") {
      helpers.insert(symbol.address);
      CHECK(symbol.binding == 0);
    } else if (function.name == "hidden") {
      hidden.insert(symbol.address);
      CHECK(symbol.binding == 0);
    } else if (function.name == "overloaded") {
      overloads.insert(symbol.name);
    }
  }
  CHECK(helpers.size() == 2);
  CHECK(hidden.size() == 2);
  CHECK(overloads == std::set<std::string>{"_Z10overloadedi", "_Z10overloadedd"});
}

TEST_CASE("real linker aliases are reported as candidates, never silently chosen") {
  const auto index = neko::elf::inspect_functions(aliases_path);
  bool found = false;
  for (const auto& match : index.functions) {
    const auto& function = index.debug.units[match.unit].functions[match.function];
    if (function.name != "implementation") {
      continue;
    }
    found = true;
    CHECK(match.status == neko::elf::match_status::ambiguous);
    REQUIRE(match.candidates.size() == 3);
    std::set<std::string> names;
    for (const auto candidate : match.candidates) {
      names.insert(index.symbols.functions[candidate].name);
    }
    CHECK(names == std::set<std::string>{"implementation", "alias_one", "alias_two"});
  }
  CHECK(found);
}

TEST_CASE("real file mutations distinguish missing, incomplete and corrupt symbols") {
  std::ifstream source(fixture_path, std::ios::binary);
  REQUIRE(source.good());
  std::vector<char> bytes{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
  REQUIRE(bytes.size() >= sizeof(Elf64_Ehdr));
  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  const auto original = neko::elf::inspect_functions(fixture_path);
  REQUIRE_FALSE(original.functions.empty());
  REQUIRE(original.functions.front().candidates.size() == 1);
  const auto& selected = original.symbols.functions[original.functions.front().candidates.front()];
  const auto section_offset = header.e_shoff + selected.table_section * header.e_shentsize;
  REQUIRE(section_offset + sizeof(Elf64_Shdr) <= bytes.size());
  Elf64_Shdr section{};
  std::memcpy(&section, bytes.data() + section_offset, sizeof(section));
  const auto symbol_offset = section.sh_offset + selected.table_index * section.sh_entsize;
  REQUIRE(symbol_offset + sizeof(Elf64_Sym) <= bytes.size());
  Elf64_Sym symbol{};
  std::memcpy(&symbol, bytes.data() + symbol_offset, sizeof(symbol));
  auto expected = neko::elf::match_status::missing_symtab;
  bool malformed = false;
  SUBCASE("missing symtab does not erase readable DWARF") {
    section.sh_type = SHT_PROGBITS;
  }
  SUBCASE("zero size") {
    symbol.st_size = 0;
    expected = neko::elf::match_status::unknown_size;
  }
  SUBCASE("size mismatch") {
    REQUIRE(symbol.st_size > 1);
    --symbol.st_size;
    expected = neko::elf::match_status::range_mismatch;
  }
  SUBCASE("missing definition") {
    symbol.st_shndx = SHN_UNDEF;
    expected = neko::elf::match_status::missing_symbol;
  }
  SUBCASE("corrupt symbol name") {
    symbol.st_name = std::numeric_limits<std::uint32_t>::max();
    malformed = true;
  }
  std::memcpy(bytes.data() + section_offset, &section, sizeof(section));
  std::memcpy(bytes.data() + symbol_offset, &symbol, sizeof(symbol));
  auto pattern = (std::filesystem::temp_directory_path() / "nekomata-index-XXXXXX").string();
  const int descriptor = mkstemp(pattern.data());
  REQUIRE(descriptor >= 0);
  close(descriptor);
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } owner{pattern};
  std::ofstream output(owner.path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  REQUIRE(output.good());
  if (malformed) {
    CHECK_THROWS_AS(neko::elf::inspect_functions(owner.path), std::runtime_error);
  } else {
    const auto index = neko::elf::inspect_functions(owner.path);
    REQUIRE(index.functions.size() == original.functions.size());
    CHECK(index.functions.front().status == expected);
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  fixture_path = argv[1];
  aliases_path = argv[2];
  doctest::Context context;
  return context.run();
}
