#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "inspect.hpp"
#include "source_fixture.hpp"

#include <limits>

namespace {

using namespace source_fixture;

neko::dwarf::binary_info inspect(const std::vector<unit>& units) {
  const sample input(build(units));
  return neko::dwarf::inspect(input.path());
}

TEST_CASE("declaration coordinates use recorded source paths without filesystem lookup") {
  unit input;
  std::string expected = "/recorded/build/unit.cpp";
  SUBCASE("compilation directory") {}
  SUBCASE("relative include directory") {
    input.includes = {"../header files"};
    input.files = {{"entry.hpp", 1}};
    expected = "/recorded/build/../header files/entry.hpp";
  }
  SUBCASE("absolute include directory") {
    input.includes = {"/other/machine/include"};
    input.files = {{"entry.hpp", 1}};
    expected = "/other/machine/include/entry.hpp";
  }
  SUBCASE("absolute filename") {
    input.includes = {"ignored"};
    input.files = {{"/absolute/file.cpp", 1}};
    expected = "/absolute/file.cpp";
  }
  SUBCASE("relative paths are not based on inspector cwd") {
    input.directory = "relative/build";
    expected = "relative/build/unit.cpp";
  }
  SUBCASE("no compilation directory") {
    input.directory.clear();
    expected = "unit.cpp";
  }
  SUBCASE("root directory does not introduce a double slash") {
    input.directory = "/";
    expected = "/unit.cpp";
  }
  const auto result = inspect({input});
  REQUIRE(result.units.size() == 1);
  REQUIRE(result.units.front().functions.size() == 1);
  const auto& location = result.units.front().functions.front().declaration;
  CHECK(location.file == expected);
  CHECK(location.line == 12);
  CHECK(location.column == 4);
}

TEST_CASE("ELF object support does not enable virtual-address DWARF inspection") {
  auto content = build({unit{}});
  content.patch(16, ET_REL, 2);
  content.patch(32, 0, 8);
  content.patch(54, 0, 2);
  content.patch(56, 0, 2);
  content.patch(content.at_number(40, 8) + 64 + 16, 0, 8);
  const sample input(content);
  CHECK_THROWS_WITH(neko::dwarf::inspect(input.path()),
                    doctest::Contains("relocatable DWARF inspection is not yet supported"));
}

TEST_CASE("missing and explicitly unspecified coordinates remain unknown") {
  unit input;
  SUBCASE("absent attributes") {
    input.nodes.front().resize(3);
  }
  SUBCASE("zero coordinates") {
    for (unsigned i = 3; i < 6; ++i)
      input.nodes.front()[i].value = 0;
  }
  const auto result = inspect({input});
  const auto& location = result.units.front().functions.front().declaration;
  CHECK_FALSE(location.file);
  CHECK_FALSE(location.line);
  CHECK_FALSE(location.column);
}

TEST_CASE("missing line table does not discard independently known line and column") {
  unit input;
  input.has_lines = false;
  const auto result = inspect({input});
  const auto& location = result.units.front().functions.front().declaration;
  CHECK_FALSE(location.file);
  CHECK(location.line == 12);
  CHECK(location.column == 4);
}

TEST_CASE("a missing or empty line section leaves only the file unavailable") {
  auto content = build({unit{}});
  const auto entry = content.at_number(40, 8) + 4 * 64;
  SUBCASE("section is absent despite statement list attribute") {
    content.patch(entry, 0, 4); // Remove the section name.
  }
  SUBCASE("section is empty") {
    content.patch(entry + 32, 0, 8);
  }
  const sample input(content);
  const auto result = neko::dwarf::inspect(input.path());
  const auto& location = result.units.front().functions.front().declaration;
  CHECK_FALSE(location.file);
  CHECK(location.line == 12);
  CHECK(location.column == 4);
}

TEST_CASE("references resolve file indexes in the originating CU, including forward references") {
  unit concrete, origin;
  concrete.files = {{"wrong.cpp", 0}};
  concrete.nodes = {{number(DW_AT_low_pc, 0x401000, DW_FORM_addr), number(DW_AT_high_pc, 4)}};
  auto kind = DW_AT_specification;
  SUBCASE("specification") {}
  SUBCASE("abstract origin") {
    kind = DW_AT_abstract_origin;
  }
  concrete.nodes.front().push_back(reference(kind, 1));
  origin.directory = "/declaration";
  origin.files = {{"api.hpp", 0}};
  origin.nodes.front().erase(origin.nodes.front().begin() + 1, origin.nodes.front().begin() + 3);
  origin.nodes.front().push_back(number(DW_AT_declaration, 1, DW_FORM_flag_present));
  const auto result = inspect({concrete, origin});
  REQUIRE(result.units.size() == 2);
  REQUIRE(result.units.front().functions.size() == 1);
  CHECK(result.units[1].functions.empty());
  const auto& function = result.units.front().functions.front();
  CHECK(function.name == "tick");
  CHECK(function.declaration.file == "/declaration/api.hpp");
  CHECK(function.declaration.line == 12);
  CHECK(function.declaration.column == 4);
}

TEST_CASE("compiler-generated functions may be named only by their linkage name") {
  unit input;
  input.nodes.front().front() = text(DW_AT_linkage_name, "_ZThn8_N4neko4tickEv");
  input.nodes.front().push_back(number(DW_AT_artificial, 1, DW_FORM_flag_present));
  const auto result = inspect({input});
  REQUIRE(result.units.front().functions.size() == 1);
  const auto& function = result.units.front().functions.front();
  CHECK(function.name == "_ZThn8_N4neko4tickEv");
  CHECK(function.linkage_name == "_ZThn8_N4neko4tickEv");
}

TEST_CASE("direct coordinate values, including zero, override inherited values") {
  unit input;
  input.nodes.push_back(function());
  input.nodes.back().erase(input.nodes.back().begin() + 1, input.nodes.back().begin() + 3);
  input.nodes.back().push_back(number(DW_AT_declaration, 1, DW_FORM_flag_present));
  input.nodes.front().push_back(reference(DW_AT_specification, 1));
  input.nodes.front()[3].value = 0;
  input.nodes.front()[4].value = 99;
  input.nodes.front()[5].value = 0;
  const auto result = inspect({input});
  const auto& location = result.units.front().functions.front().declaration;
  CHECK_FALSE(location.file);
  CHECK(location.line == 99);
  CHECK_FALSE(location.column);
}

TEST_CASE("invalid coordinate forms and source file indexes fail inspection") {
  unit input;
  SUBCASE("file index out of bounds") {
    input.nodes.front()[3].value = 2;
  }
  SUBCASE("file index too large for signed API") {
    input.nodes.front()[3] =
        number(DW_AT_decl_file, std::numeric_limits<std::uint64_t>::max(), DW_FORM_data8);
  }
  SUBCASE("invalid directory index") {
    input.files.front().directory = 2;
  }
  SUBCASE("negative coordinate") {
    input.nodes.front()[4] =
        number(DW_AT_decl_line, std::numeric_limits<std::uint64_t>::max(), DW_FORM_sdata);
  }
  SUBCASE("string coordinate") {
    input.nodes.front()[4] = text(DW_AT_decl_line, "12");
  }
  SUBCASE("nonconstant coordinate") {
    input.nodes.front()[4] = number(DW_AT_decl_line, 12, DW_FORM_sec_offset);
  }
  CHECK_THROWS_AS(inspect({input}), std::runtime_error);
}

TEST_CASE("a DWARF 5 line table leaves the coordinate unknown, not the inspection failed") {
  // The DWARF 5 header describes its entries with a format list and keeps
  // strings in a separate section; until that is parsed, the honest answer for
  // a file index is "unknown" — the same answer a missing line table already
  // gives. Failing instead would take the functions down with it, and the
  // functions are what a symbol map needs.
  unit input;
  input.line_version = 5;
  const auto info = inspect({input});
  REQUIRE(info.units.size() == 1);
  REQUIRE(info.units.front().functions.size() == 1);
  CHECK(info.units.front().functions.front().declaration.file == std::nullopt);
}

TEST_CASE("missing coordinates cannot recurse indefinitely through malformed references") {
  unit input;
  input.nodes.front().resize(3);
  SUBCASE("self cycle") {
    input.nodes.front().push_back(reference(DW_AT_specification, 0));
  }
  SUBCASE("dangling reference") {
    input.nodes.front().push_back(reference(DW_AT_specification, 9));
  }
  CHECK_THROWS_AS(inspect({input}), std::runtime_error);
}

TEST_CASE("signed positive coordinate constants are supported") {
  unit input;
  input.nodes.front()[4] = number(DW_AT_decl_line, 31, DW_FORM_sdata);
  CHECK(inspect({input}).units.front().functions.front().declaration.line == 31);
}

TEST_CASE("source filename lookup is cached for repeated references") {
  unit input;
  input.files.resize(4000);
  input.nodes.front()[3].value = 4000;
  input.nodes.resize(3000, input.nodes.front());
  const auto result = inspect({input});
  REQUIRE(result.units.front().functions.size() == 3000);
  CHECK(result.units.front().functions.back().declaration.file == "/recorded/build/unit.cpp");
}

TEST_CASE("distinct file indexes use direct lookup without quadratic traversal") {
  unit input;
  input.files.resize(4100);
  input.nodes.resize(4100, input.nodes.front());
  for (std::size_t i = 0; i < input.nodes.size(); ++i) {
    input.nodes[i][3].value = i + 1;
  }
  const auto result = inspect({input});
  REQUIRE(result.units.front().functions.size() == 4100);
  CHECK(result.units.front().functions.back().declaration.file == "/recorded/build/unit.cpp");
}

TEST_CASE("source table entries are bounded across compilation units") {
  unit input;
  input.files.resize(500001);
  CHECK_THROWS_WITH(inspect({input, input}), doctest::Contains("inspection entry limit"));
}

TEST_CASE("shared line tables still have a file-wide header allocation budget") {
  std::vector<unit> units(65);
  // Only the first short filename is referenced. A large unused entry tests
  // header allocation independently of copied path/name budgets.
  units.front().files.push_back({std::string(1024 * 1024, 'x'), 0});
  for (std::size_t i = 1; i < units.size(); ++i) {
    units[i].statement_offset = 0;
  }
  CHECK_THROWS_WITH(inspect(units),
                    doctest::Contains("source headers exceed inspection byte limit"));
}

TEST_CASE("declaration lookup never interprets the line program") {
  unit input;
  SUBCASE("many emitted rows") {
    input.line_program.resize(500000, DW_LNS_copy);
  }
  SUBCASE("invalid row opcode is outside header-only inspection") {
    input.line_program = {};
    input.line_program.number(0, 1);
    input.line_program.leb(1000); // Truncated extended opcode, if interpreted.
  }
  CHECK(inspect({input}).units.front().functions.front().declaration.file ==
        "/recorded/build/unit.cpp");
}

TEST_CASE("files defined only by the line program cannot resolve declaration indexes") {
  unit input;
  bytes extension;
  extension.number(DW_LNE_define_file, 1);
  extension.text("dynamic.cpp");
  extension.leb(0);
  extension.leb(0);
  extension.leb(0);
  input.line_program.number(0, 1);
  input.line_program.leb(extension.size());
  input.line_program.append(extension);
  input.nodes.front()[3].value = 2;
  CHECK_THROWS_WITH(inspect({input}), doctest::Contains("outside the source file table"));
}

TEST_CASE("malformed source headers are rejected before reading beyond their bounds") {
  const auto original = build({unit{}});
  auto lines = section_data(original, 4);
  SUBCASE("DWARF64 marker") {
    lines.patch(0, 0xffffffff, 4);
  }
  SUBCASE("reserved unit length") {
    lines.patch(0, 0xfffffff0, 4);
  }
  SUBCASE("unit length outside section") {
    lines.patch(0, lines.size(), 4);
  }
  SUBCASE("header length outside unit") {
    lines.patch(6, lines.size(), 4);
  }
  SUBCASE("zero header length is malformed, not missing metadata") {
    lines.patch(6, 0, 4);
  }
  SUBCASE("header cannot consume line program terminators") {
    lines.patch(6, lines.at_number(6, 4) - 1, 4);
  }
  SUBCASE("truncated prefix") {
    lines.resize(9);
  }
  SUBCASE("missing filename terminator") {
    lines.resize(32);
    lines.patch(0, lines.size() - 4, 4);
    lines.patch(6, lines.size() - 10, 4);
  }
  SUBCASE("zero opcode base") {
    lines.patch(15, 0, 1);
  }
  SUBCASE("unexpected header padding") {
    lines.patch(6, lines.at_number(6, 4) + 1, 4);
  }
  const sample input(elf(section_data(original, 2), section_data(original, 3), lines));
  CHECK_THROWS_AS(neko::dwarf::inspect(input.path()), std::runtime_error);
}

TEST_CASE("overflowing source table ULEB128 is rejected") {
  unit input;
  input.files.front().directory = std::numeric_limits<std::uint64_t>::max();
  const auto original = build({input});
  auto lines = section_data(original, 4);
  // Fixed prefix/opcodes (28), directory terminator (1), and filename (9).
  lines.patch(38 + 9, 2, 1); // The tenth ULEB128 byte may only be zero or one.
  const sample image(elf(section_data(original, 2), section_data(original, 3), lines));
  CHECK_THROWS_WITH(neko::dwarf::inspect(image.path()), doctest::Contains("ULEB128"));
}

TEST_CASE("source path duplication is bounded across functions and compilation units") {
  unit input;
  input.files.front().name.assign(128 * 1024, 'x');
  input.nodes.resize(300, input.nodes.front());
  CHECK_THROWS_WITH(inspect({input, input}), doctest::Contains("inspection byte limit"));
}

} // namespace
