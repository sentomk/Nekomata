#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "binary_file.hpp"

#include <elf.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <string>

namespace {

std::array<std::filesystem::path, 4> fixtures;

TEST_CASE("compiler-produced objects preserve local functions and code section identity") {
  std::set<std::string> exports;
  std::set<std::string> locals;
  for (std::size_t i = 0; i < fixtures.size(); ++i) {
    CAPTURE(fixtures[i]);
    const neko::elf::binary_file file(fixtures[i]);
    CHECK(file.kind() == neko::elf::binary_kind::relocatable);
    const auto symbols = file.symbols();
    CHECK(symbols.kind == neko::elf::binary_kind::relocatable);
    REQUIRE(symbols.has_symtab);
    REQUIRE(symbols.functions.size() == 2);
    std::set<std::uint32_t> sections;
    std::set<std::uint64_t> offsets;
    std::size_t local_count = 0;
    for (const auto& symbol : symbols.functions) {
      CHECK(symbol.size > 0);
      CHECK(symbol.table_index > 0);
      CHECK(symbol.section != SHN_UNDEF);
      sections.insert(symbol.section);
      offsets.insert(symbol.address);
      if (symbol.binding == STB_LOCAL) {
        ++local_count;
        locals.insert(symbol.name);
      } else {
        CHECK(symbol.binding == STB_GLOBAL);
        exports.insert(symbol.name);
      }
    }
    CHECK(local_count == 1);
    if (i < 2) {
      CHECK(sections.size() == 1);
      CHECK(offsets.size() == 2);
      CHECK(*offsets.begin() == 0);
    } else {
      CHECK(sections.size() == 2);
      CHECK(offsets == std::set<std::uint64_t>{0});
    }
  }
  CHECK(locals == std::set<std::string>{"_ZL6helperi"});
  CHECK(exports == std::set<std::string>{"_Z6from_ai", "_Z6from_bi"});
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
