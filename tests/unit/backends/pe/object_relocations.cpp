#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "object_file.hpp"

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

bool has_symbol(const neko::pe::object_file& obj, const std::string& name) {
  for (const auto& sym : obj.symbols) {
    if (!sym.auxiliary && sym.name == name && sym.section_number >= 1) {
      return true;
    }
  }
  return false;
}

const neko::pe::symbol* find_symbol(const neko::pe::object_file& obj, const std::string& name) {
  for (const auto& sym : obj.symbols) {
    if (!sym.auxiliary && sym.name == name && sym.section_number >= 1) {
      return &sym;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("decodable sites stay inside their sections and symbol table") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto obj = parse(path);
    for (const auto& rel : obj.relocations) {
      CHECK(rel.target_section < obj.sections.size());
      CHECK(rel.symbol_index < obj.symbols.size());
      CHECK(rel.offset < obj.sections[rel.target_section].size);
      CHECK(rel.kind != neko::pe::relocation_kind::unsupported);
      if (rel.kind == neko::pe::relocation_kind::relative_32) {
        CHECK(rel.rel32_bias <= 5);
      }
    }
  }
}

TEST_CASE("text calls decode to REL32 and data pointers to ADDR64") {
  const auto obj = parse(fixtures[0]); // relocs.c

  bool saw_relative_call = false;
  for (const auto& rel : obj.relocations) {
    const auto& site = obj.sections[rel.target_section];
    if (rel.kind == neko::pe::relocation_kind::relative_32 &&
        site.cls == neko::pe::section_class::text) {
      saw_relative_call = true;
      CHECK(rel.rel32_bias == 0); // calls and plain RIP-relative loads
    }
  }
  CHECK(saw_relative_call);

  // The initialized pointer global carries one ADDR64 against its target;
  // metadata sections (MSVC's .rtc$*) may carry more, against their own.
  std::size_t pointer_relocs = 0;
  for (const auto& rel : obj.relocations) {
    if (rel.kind == neko::pe::relocation_kind::absolute_64 &&
        obj.symbols[rel.symbol_index].name == "reloc_global") {
      const auto& site = obj.sections[rel.target_section];
      CHECK(site.cls == neko::pe::section_class::data);
      ++pointer_relocs;
    }
  }
  CHECK(pointer_relocs == 1);
}

TEST_CASE("unwind entries associate pdata with text bounds and xdata") {
  for (const auto& path : fixtures) {
    CAPTURE(path);
    const auto obj = parse(path);
    REQUIRE(obj.unwind_table.size() > 0);

    for (const auto& entry : obj.unwind_table) {
      const auto& text = obj.sections[entry.text_section];
      const auto& xdata = obj.sections[entry.xdata_section];
      const auto& pdata = obj.sections[entry.pdata_section];
      CHECK(text.cls == neko::pe::section_class::text);
      CHECK(xdata.name == ".xdata");
      CHECK(pdata.name == ".pdata");
      CHECK(entry.begin_offset < entry.end_offset);
      CHECK(entry.end_offset <= text.size);
      CHECK(entry.unwind_offset < xdata.size);
    }

    // A function that calls is never a leaf, so its defining section —
    // plain .text or a COMDAT — must carry at least one entry.
    const char* callers[] = {"reloc_call_site", "?unwind_probe@@YAHH@Z"};
    for (const char* caller : callers) {
      const auto* sym = find_symbol(obj, caller);
      if (sym == nullptr) {
        continue; // the C name is absent from the C++ fixture and vice versa
      }
      bool covered = false;
      for (const auto& entry : obj.unwind_table) {
        covered =
            covered || entry.text_section == static_cast<std::uint16_t>(sym->section_number - 1);
      }
      CHECK(covered);
    }
  }
}

TEST_CASE("debug streams decode SECREL and SECTION against other sections") {
  const auto obj = parse(fixtures[2]); // shapes.cpp with debug info
  REQUIRE(has_symbol(obj, "?unwind_probe@@YAHH@Z"));

  bool saw_section_relative = false;
  bool saw_section_index = false;
  for (const auto& rel : obj.relocations) {
    if (rel.kind == neko::pe::relocation_kind::section_relative_32) {
      saw_section_relative = true;
      CHECK(obj.sections[rel.target_section].cls == neko::pe::section_class::other);
    }
    if (rel.kind == neko::pe::relocation_kind::section_index_16) {
      saw_section_index = true;
      CHECK(obj.sections[rel.target_section].cls == neko::pe::section_class::other);
    }
  }
  CHECK(saw_section_relative);
  CHECK(saw_section_index);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
