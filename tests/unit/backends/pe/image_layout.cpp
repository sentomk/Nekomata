#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "image_layout.hpp"
#include "object_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// relocs.c, shapes.cpp, shapes.cpp compiled with debug streams.
std::array<std::filesystem::path, 3> fixtures;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), {}};
}

neko::pe::object_file parse(const std::filesystem::path& path) {
  const auto bytes = read_file(path);
  return neko::pe::parse_object(bytes.data(), bytes.size());
}

constexpr std::size_t sizeof_header = 20;
constexpr std::size_t sizeof_section_header = 40;

void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
  out[at] = static_cast<std::uint8_t>(value);
  out[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}

// A header plus section headers for the fields the planner inspects.
std::vector<std::uint8_t> object_with_sections(std::initializer_list<std::string> names,
                                               std::uint32_t flags) {
  std::vector<std::uint8_t> out(sizeof_header + names.size() * sizeof_section_header, 0);
  put16(out, 0, 0x8664);
  put16(out, 2, static_cast<std::uint16_t>(names.size()));
  std::size_t at = sizeof_header;
  for (const auto& name : names) {
    std::copy_n(name.begin(), std::min(name.size(), std::size_t{8}), out.begin() + at);
    put32(out, at + 16, 0);     // SizeOfRawData
    put32(out, at + 36, flags); // Characteristics
    at += sizeof_section_header;
  }
  return out;
}

const neko::pe::section_placement* find_placement(const neko::pe::image_layout& plan,
                                                  std::uint16_t section) {
  for (const auto& p : plan.code) {
    if (p.section == section) {
      return &p;
    }
  }
  for (const auto& p : plan.unwind) {
    if (p.section == section) {
      return &p;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("code sections place in order with honored alignment and no overlap") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto obj = parse(path);
    const auto plan = neko::pe::plan_image(obj);

    REQUIRE(plan.code.size() > 0);
    CHECK(
        std::is_sorted(plan.code.begin(), plan.code.end(), [](const auto& left, const auto& right) {
          return left.offset < right.offset;
        }));

    std::uint64_t cursor = 0;
    for (const auto& p : plan.code) {
      const auto& sec = obj.sections[p.section];
      const bool loadable =
          sec.cls == neko::pe::section_class::text || sec.cls == neko::pe::section_class::rodata;
      CHECK(loadable);
      CHECK(p.offset >= cursor);
      CHECK(p.offset % std::max<std::uint64_t>(16, sec.align) == 0);
      CHECK(p.size == sec.size);
      cursor = p.offset + p.size;
    }

    // The unwind tail follows every code section, .pdata-sized in entries.
    for (const auto& p : plan.unwind) {
      const auto& sec = obj.sections[p.section];
      CHECK((sec.name == ".pdata" || sec.name == ".xdata"));
      CHECK(p.offset >= cursor);
      CHECK(p.offset % std::max<std::uint64_t>(4, sec.align) == 0);
      if (sec.name == ".pdata") {
        CHECK(p.size % 12 == 0);
      }
      cursor = p.offset + p.size;
    }
    CHECK(plan.image_size >= cursor);
    CHECK(plan.image_size < std::uint64_t{1} << 32);
  }
}

TEST_CASE("every defined text function lands inside its section") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto obj = parse(path);
    const auto plan = neko::pe::plan_image(obj);

    std::size_t text_functions = 0;
    for (std::uint32_t index = 0; index < obj.symbols.size(); ++index) {
      const auto& sym = obj.symbols[index];
      if (sym.auxiliary || !sym.is_function() || sym.section_number < 1) {
        continue;
      }
      if (obj.sections[static_cast<std::size_t>(sym.section_number) - 1].cls !=
          neko::pe::section_class::text) {
        continue;
      }
      ++text_functions;
      const auto candidate = std::find_if(plan.functions.begin(), plan.functions.end(),
                                          [&](const auto& f) { return f.symbol_index == index; });
      REQUIRE(candidate != plan.functions.end());
      const auto* placement =
          find_placement(plan, static_cast<std::uint16_t>(sym.section_number - 1));
      REQUIRE(placement != nullptr);
      CHECK(candidate->offset_in_image >= placement->offset);
      CHECK(candidate->offset_in_image < placement->offset + placement->size);
    }
    CHECK(plan.functions.size() == text_functions);
  }
}

TEST_CASE("undefined externals reserve one trampoline slot each") {
  // MSVC's Debug defaults add runtime-check instrumentation referencing CRT
  // helpers; those are externals like any other (the live process resolves
  // them). Semantic externals are the fixture's own declarations.
  const auto instrumentation = [](const std::string& name) {
    return name.starts_with("_RTC_") || name.starts_with("__");
  };

  const auto obj = parse(fixtures[0]); // relocs.c calls reloc_external
  const auto plan = neko::pe::plan_image(obj);

  std::size_t semantic = 0;
  for (const auto& slot : plan.trampolines) {
    if (slot.name == "reloc_external") {
      ++semantic;
    } else {
      CHECK(instrumentation(slot.name));
    }
  }
  CHECK(semantic == 1);

  std::uint64_t sections_end = 0;
  for (const auto& p : plan.code) {
    sections_end = std::max(sections_end, p.offset + p.size);
  }
  for (const auto& p : plan.unwind) {
    sections_end = std::max(sections_end, p.offset + p.size);
  }
  for (const auto& slot : plan.trampolines) {
    CHECK(slot.offset_in_image >= sections_end); // slots follow every section
    CHECK(slot.offset_in_image + 13 <= plan.image_size);
  }

  // shapes.cpp declares no semantic externals: instrumentation only.
  const auto shapes = parse(fixtures[1]);
  for (const auto& slot : neko::pe::plan_image(shapes).trampolines) {
    CHECK(instrumentation(slot.name));
  }
}

TEST_CASE("objects without code or with impossible alignment are rejected") {
  const std::uint32_t code_flags = 0x60000020; // CNT_CODE | MEM_EXECUTE | MEM_READ

  auto no_code = object_with_sections({".debug$S"}, 0x42100040);
  const auto no_code_obj = neko::pe::parse_object(no_code.data(), no_code.size());
  CHECK_THROWS_WITH(neko::pe::plan_image(no_code_obj), doctest::Contains("no code"));

  auto over_aligned = object_with_sections({".text"}, code_flags | (13u << 20)); // 8192
  const auto over_obj = neko::pe::parse_object(over_aligned.data(), over_aligned.size());
  CHECK_THROWS_WITH(neko::pe::plan_image(over_obj), doctest::Contains("alignment"));
}

int main(int argc, char** argv) {
  if (argc != 4) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
