#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "object_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::array<std::filesystem::path, 2> fixtures;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), {}};
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

constexpr std::size_t sizeof_header = 20;
constexpr std::size_t sizeof_section_header = 40;

// A minimal header plus an optional section header, the fields the reader
// inspects.
std::vector<std::uint8_t> object_with(std::uint16_t machine, std::uint16_t section_count,
                                      std::uint32_t symbol_table, std::string_view name_field) {
  std::vector<std::uint8_t> out(sizeof_header + (name_field.empty() ? 0 : sizeof_section_header),
                                0);
  put16(out, 0, machine);
  put16(out, 2, section_count);
  put32(out, 8, symbol_table);
  std::copy_n(name_field.begin(), std::min(name_field.size(), std::size_t{8}),
              out.begin() + sizeof_header);
  return out;
}

} // namespace

TEST_CASE("compiler-produced objects classify loadable sections") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto bytes = read_file(path);
    const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());

    REQUIRE(obj.sections.size() > 0);
    bool saw_text = false;
    bool saw_initialized_data = false;
    for (std::size_t i = 0; i < obj.sections.size(); ++i) {
      const auto& sec = obj.sections[i];
      // The index is the identity: names repeat across and within objects.
      CHECK(sec.index == i);
      CHECK(sec.align != 0);
      CHECK((sec.align & (sec.align - 1)) == 0);
      if (sec.cls == neko::pe::section_class::text) {
        saw_text = true;
        CHECK(sec.size > 0);
        CHECK(sec.bytes.size() > 0);
        CHECK(sec.bytes.size() <= sec.size);
      } else if (sec.cls == neko::pe::section_class::data) {
        // Uninitialized storage keeps its size with no file bytes.
        CHECK(sec.size > 0);
        if (!sec.bytes.empty()) {
          saw_initialized_data = true;
          CHECK(sec.bytes.size() <= sec.size);
        }
      } else if (sec.cls == neko::pe::section_class::rodata) {
        CHECK(sec.bytes.size() > 0);
      } else {
        CHECK(sec.bytes.empty());
        CHECK(!sec.name.empty());
      }
      // Unwind tables carry image-relative entries that cannot enter the
      // arena; both drivers emit them even at /Od without exceptions.
      if (sec.name == ".pdata" || sec.name == ".xdata") {
        CHECK(sec.cls == neko::pe::section_class::other);
      }
    }
    CHECK(saw_text);
    CHECK(saw_initialized_data);
  }
}

TEST_CASE("warm_counter is the first initialized global on both drivers") {
  const auto bytes = read_file(fixtures[0]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
  bool found = false;
  for (const auto& sec : obj.sections) {
    if (sec.cls == neko::pe::section_class::data && !sec.bytes.empty() && sec.bytes.size() >= 4) {
      REQUIRE(!found); // exactly one initialized writable section
      found = true;
      CHECK(sec.bytes[0] == 7); // int 7, little-endian, at offset 0
      CHECK(sec.bytes[1] == 0);
      CHECK(sec.bytes[2] == 0);
      CHECK(sec.bytes[3] == 0);
    }
  }
  CHECK(found);
}

TEST_CASE("inline functions place text in COMDAT sections on both drivers") {
  const auto bytes = read_file(fixtures[1]);
  const auto obj = neko::pe::parse_object(bytes.data(), bytes.size());
  std::size_t comdat_text = 0;
  for (const auto& sec : obj.sections) {
    if (sec.comdat && sec.cls == neko::pe::section_class::text) {
      ++comdat_text;
    }
  }
  CHECK(comdat_text > 0);
}

TEST_CASE("unsupported and corrupt objects are rejected with reasons") {
  const std::uint16_t amd64 = 0x8664;

  // Mach-O and ELF magics decode into foreign machine types.
  std::vector<std::uint8_t> mach_o(sizeof_header, 0);
  mach_o[0] = 0xce;
  mach_o[1] = 0xfa;
  mach_o[2] = 0xed;
  mach_o[3] = 0xfe;
  CHECK_THROWS_WITH(neko::pe::parse_object(mach_o.data(), mach_o.size()),
                    doctest::Contains("not x86-64"));

  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(0x014c, 0, 0, "").data(), 20),
                    doctest::Contains("not x86-64"));

  // Sig1 == 0 is the bigobj signature rather than a machine type.
  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(0, 1, 0, "").data(), 20),
                    doctest::Contains("bigobj"));

  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(amd64, 0, 0, "").data(), 20),
                    doctest::Contains("no sections"));

  // A claimed section header beyond the buffer.
  std::vector<std::uint8_t> truncated = object_with(amd64, 1, 0, "");
  truncated.resize(sizeof_header + 1);
  CHECK_THROWS_WITH(neko::pe::parse_object(truncated.data(), truncated.size()),
                    doctest::Contains("truncated"));

  // A long section name needs a symbol table to locate the string table.
  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(amd64, 1, 0, "/4096").data(), 60),
                    doctest::Contains("without a symbol table"));

  // A long name whose offset leaves the file.
  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(amd64, 1, 20, "/9999").data(), 60),
                    doctest::Contains("truncated"));

  // A name that is not a decimal offset.
  CHECK_THROWS_WITH(neko::pe::parse_object(object_with(amd64, 1, 20, "/nope").data(), 60),
                    doctest::Contains("not decimal"));
}

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
