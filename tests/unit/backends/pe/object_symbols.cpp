#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "object_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

std::array<std::filesystem::path, 2> fixtures;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), {}};
}

std::uint32_t symbol_count_of(const std::vector<std::uint8_t>& bytes) {
  REQUIRE(bytes.size() >= 20);
  std::uint32_t count = 0;
  std::memcpy(&count, bytes.data() + 12, 4);
  return count;
}

const neko::pe::symbol* find(const neko::pe::object_file& obj, const std::string& name) {
  const auto it = std::find_if(obj.symbols.begin(), obj.symbols.end(),
                               [&](const neko::pe::symbol& s) { return s.name == name; });
  return it == obj.symbols.end() ? nullptr : &*it;
}

constexpr std::uint8_t class_external = 2;
constexpr std::uint8_t class_static = 3;

neko::pe::section_class section_of(const neko::pe::object_file& obj, const neko::pe::symbol& sym) {
  REQUIRE(sym.section_number >= 1);
  REQUIRE(obj.sections.size() >= static_cast<std::size_t>(sym.section_number));
  return obj.sections[static_cast<std::size_t>(sym.section_number) - 1].cls;
}

// Appends an 18-byte symbol record with an inline name of at most eight
// bytes, returning the record start for optional patching.
std::size_t add_symbol(std::vector<std::uint8_t>& out, std::string_view name, std::int16_t section,
                       std::uint8_t storage_class, std::uint8_t aux_count) {
  const std::size_t at = out.size();
  out.resize(out.size() + 18, 0);
  std::copy_n(name.begin(), std::min(name.size(), std::size_t{8}), out.begin() + at);
  const std::uint16_t section_raw = static_cast<std::uint16_t>(section);
  out[at + 8] = 0; // value
  out[at + 12] = static_cast<std::uint8_t>(section_raw);
  out[at + 13] = static_cast<std::uint8_t>(section_raw >> 8);
  out[at + 16] = storage_class;
  out[at + 17] = aux_count;
  return at;
}

void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
  out[at] = static_cast<std::uint8_t>(value);
  out[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

// Header with one section plus a symbol table area the caller appends to.
std::vector<std::uint8_t> object_shell() {
  std::vector<std::uint8_t> out(20 + 40, 0);
  put16(out, 0, 0x8664); // x86-64
  put16(out, 2, 1);      // one section
  std::copy_n(".text", 5, out.begin() + 20);
  put32(out, 20 + 36, 0x60000020); // code | execute
  return out;
}

} // namespace

TEST_CASE("function symbols carry their marking and live in text sections") {
  const auto bytes = read_file(fixtures[0]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
  for (const char* name : {"add_probe", "call_probe", "label_probe", "address_probe"}) {
    CAPTURE(name);
    const auto* sym = find(obj, name);
    REQUIRE(sym != nullptr);
    CHECK(sym->is_function());
    CHECK(sym->storage_class == class_external);
    CHECK(sym->section_number >= 1);
    CHECK(section_of(obj, *sym) == neko::pe::section_class::text);
  }
  // Undefined references stay undefined on both drivers; the function
  // marking of a mere reference varies (MSVC sets it, clang does not).
  const auto* external = find(obj, "external_source");
  REQUIRE(external != nullptr);
  CHECK(external->storage_class == class_external);
  CHECK(external->section_number == 0);
  CHECK(!external->is_common());
}

TEST_CASE("uninitialized globals arrive as common or bss definitions") {
  const auto bytes = read_file(fixtures[0]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());

  // MSVC emits a common symbol (undefined, value = storage size); clang
  // defines the variable in an uninitialized section.
  const auto* cold = find(obj, "cold_counter");
  REQUIRE(cold != nullptr);
  CHECK(cold->storage_class == class_external);
  const bool common_msvc = cold->is_common() && cold->value == 4;
  const bool defined_clang = !cold->is_common() && cold->section_number >= 1 &&
                             section_of(obj, *cold) == neko::pe::section_class::data;
  if (!common_msvc) {
    CHECK(defined_clang);
  }

  const auto* warm = find(obj, "warm_counter");
  REQUIRE(warm != nullptr);
  CHECK(!warm->is_function());
  CHECK(warm->section_number >= 1);
  CHECK(section_of(obj, *warm) == neko::pe::section_class::data);
}

TEST_CASE("MSVC-mangled C++ names pass through byte-exact") {
  const auto bytes = read_file(fixtures[1]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
  for (const char* name : {"?inline_scale@@YAHH@Z", "??0widget@@QEAA@H@Z", "??1widget@@QEAA@XZ",
                           "?method_value@@YAHAEBUwidget@@@Z", "?unwind_probe@@YAHH@Z"}) {
    CAPTURE(name);
    const auto* sym = find(obj, name);
    REQUIRE(sym != nullptr);
    CHECK(sym->is_function());
    CHECK(sym->storage_class == class_external);
  }
  const auto* cold = find(obj, "?cold_counter@@3HA");
  REQUIRE(cold != nullptr);
  CHECK(!cold->is_function());
  CHECK(cold->section_number >= 1);

  // selectany folds through a COMDAT data section on both drivers.
  const auto* shared = find(obj, "?shared_counter@@3HA");
  REQUIRE(shared != nullptr);
  REQUIRE(shared->section_number >= 1);
  const auto& sec = obj.sections[static_cast<std::size_t>(shared->section_number) - 1];
  CHECK(sec.comdat);
  CHECK(sec.cls == neko::pe::section_class::data);
  CHECK(sec.selection == neko::pe::comdat_selection::any);
}

TEST_CASE("symbol slots stay aligned with the COFF table") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto bytes = read_file(path);
    const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
    // Auxiliary records occupy their table slots as placeholders, so the
    // vector lines up with the table indexes relocations reference.
    CHECK(obj.symbols.size() == symbol_count_of(bytes));
    CHECK(std::any_of(obj.symbols.begin(), obj.symbols.end(),
                      [](const neko::pe::symbol& s) { return s.auxiliary; }));
    // Marker symbols pass through unfiltered (policy is loader business).
    CHECK(std::any_of(obj.symbols.begin(), obj.symbols.end(), [](const neko::pe::symbol& s) {
      return s.section_number == -1 && !s.name.empty() && s.name[0] == '@';
    }));
  }
}

TEST_CASE("COMDAT selections and associations survive the parse") {
  const auto bytes = read_file(fixtures[1]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
  std::size_t comdat_text = 0;
  for (const auto& sec : obj.sections) {
    if (sec.comdat) {
      // A flagged section always carries its folding contract.
      CHECK(sec.selection != neko::pe::comdat_selection::none);
      if (sec.selection == neko::pe::comdat_selection::associative) {
        CHECK(sec.association >= 1);
        CHECK(sec.association <= obj.sections.size());
      }
    }
    if (sec.comdat && sec.cls == neko::pe::section_class::text) {
      ++comdat_text;
    }
  }
  // Inline function bodies, on every driver.
  CHECK(comdat_text > 0);
}

TEST_CASE("malformed symbol tables are rejected with reasons") {
  SUBCASE("count without an offset") {
    auto out = object_shell();
    put32(out, 8, 0);  // symbol table offset
    put32(out, 12, 4); // ... and a nonzero count
    CHECK_THROWS_WITH(neko::pe::parse_object(out.data(), out.size()),
                      doctest::Contains("symbol table offset missing"));
  }

  SUBCASE("table beyond the file") {
    auto out = object_shell();
    put32(out, 8, 4000);
    put32(out, 12, 4);
    CHECK_THROWS_WITH(neko::pe::parse_object(out.data(), out.size()),
                      doctest::Contains("symbol table"));
  }

  SUBCASE("auxiliary records overrun the table") {
    auto out = object_shell();
    put32(out, 8, 60);
    put32(out, 12, 2);
    add_symbol(out, "overrun", 1, class_static, 5);  // claims five aux slots
    add_symbol(out, "filler", 0, class_external, 0); // the table itself is whole
    CHECK_THROWS_WITH(neko::pe::parse_object(out.data(), out.size()),
                      doctest::Contains("auxiliary records run past"));
  }

  SUBCASE("section number beyond the section table") {
    auto out = object_shell();
    put32(out, 8, 60);
    put32(out, 12, 1);
    add_symbol(out, "lost", 5, class_external, 0);
    CHECK_THROWS_WITH(neko::pe::parse_object(out.data(), out.size()),
                      doctest::Contains("section number out of bounds"));
  }

  SUBCASE("unknown COMDAT selection") {
    auto out = object_shell();
    put32(out, 8, 60);
    put32(out, 12, 2);
    const std::size_t record = add_symbol(out, ".text", 1, class_static, 1); // the section symbol
    add_symbol(out, "", 0, 0, 0);                                            // its auxiliary record
    put16(out, record + 18 + 14, 9); // selection byte, invalid
    CHECK_THROWS_WITH(neko::pe::parse_object(out.data(), out.size()),
                      doctest::Contains("unknown COMDAT selection"));
  }
}

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
