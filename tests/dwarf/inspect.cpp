#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "inspect.hpp"

#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path fixture_path;
std::filesystem::path reference_path;

class scratch_file {
public:
  scratch_file() {
    auto pattern = (std::filesystem::temp_directory_path() / "nekomata-inspect-XXXXXX").string();
    if (!mkdtemp(pattern.data())) {
      throw std::runtime_error("cannot create test directory");
    }
    directory_ = pattern;
  }
  ~scratch_file() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  scratch_file(const scratch_file&) = delete;
  scratch_file& operator=(const scratch_file&) = delete;

  std::filesystem::path path() const { return directory_ / "sample with spaces"; }

  void write(const std::vector<char>& bytes) const {
    std::ofstream out(path(), std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      throw std::runtime_error("cannot write test fixture");
    }
  }

private:
  std::filesystem::path directory_;
};

std::vector<char> fixture_bytes() {
  std::ifstream input(fixture_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read fixture");
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const neko::dwarf::compilation_unit& unit_named(const neko::dwarf::binary_info& binary,
                                                std::string_view name) {
  const auto found = std::find_if(binary.units.begin(), binary.units.end(), [&](const auto& unit) {
    return std::filesystem::path(unit.name).filename() == name;
  });
  REQUIRE(found != binary.units.end());
  return *found;
}

const neko::dwarf::function_record& function_named(const neko::dwarf::compilation_unit& unit,
                                                   std::string_view name) {
  const auto found = std::find_if(unit.functions.begin(), unit.functions.end(),
                                  [&](const auto& fn) { return fn.name == name; });
  REQUIRE(found != unit.functions.end());
  return *found;
}

TEST_CASE("compilation units keep same-name local functions separate") {
  const auto binary = neko::dwarf::inspect(fixture_path);
  CHECK(binary.path.is_absolute());
  REQUIRE(binary.units.size() == 3);
  const auto& a = unit_named(binary, "a.cpp");
  const auto& b = unit_named(binary, "b.cpp");
  CHECK_FALSE(a.directory.empty());
  CHECK_FALSE(a.producer.empty());
  CHECK(a.die_offset != b.die_offset);
  for (const auto* name : {"helper", "hidden"}) {
    const auto& first = function_named(a, name);
    const auto& second = function_named(b, name);
    CHECK(first.die_offset != second.die_offset);
    REQUIRE(first.ranges.size() == 1);
    REQUIRE(second.ranges.size() == 1);
    CHECK(first.ranges.front().begin != second.ranges.front().begin);
  }
  CHECK(function_named(a, "from_a").linkage_name == "_Z6from_av");
  CHECK(function_named(b, "from_b").linkage_name == "_Z6from_bv");
  CHECK(function_named(unit_named(binary, "main.cpp"), "main").name == "main");
}

TEST_CASE("overloads and out-of-line member definitions retain linkage names") {
  const auto binary = neko::dwarf::inspect(fixture_path);
  const auto& unit = unit_named(binary, "a.cpp");
  std::set<std::string> overloads;
  for (const auto& function : unit.functions) {
    if (function.name == "overloaded") {
      REQUIRE(function.linkage_name.has_value());
      overloads.insert(*function.linkage_name);
    }
  }
  CHECK(overloads == std::set<std::string>{"_Z10overloadedi", "_Z10overloadedd"});
  // GCC emits DW_AT_specification for this out-of-line definition.
  CHECK(function_named(unit, "compute").linkage_name == "_ZNK6sample6widget7computeEv");
}

TEST_CASE("compiler-produced declaration coordinates distinguish sources and overloads") {
  const auto binary = neko::dwarf::inspect(fixture_path);
  const auto& a = unit_named(binary, "a.cpp");
  const auto& b = unit_named(binary, "b.cpp");
  for (const auto* unit : {&a, &b}) {
    const auto& source = function_named(*unit, "helper").declaration;
    REQUIRE(source.file.has_value());
    CHECK(std::filesystem::path(*source.file).filename() ==
          std::filesystem::path(unit->name).filename());
    CHECK(source.line == 3);
    // GCC emits columns here; Clang can omit them. Omission is not column 0.
    if (source.column) {
      CHECK(*source.column > 0);
    }
  }
  const auto& member = function_named(a, "compute").declaration;
  REQUIRE(member.file.has_value());
  CHECK(std::filesystem::path(*member.file).filename() == "a.cpp");
  CHECK(member.line == 21);
  std::set<std::uint64_t> overload_lines;
  for (const auto& function : a.functions) {
    if (function.name == "overloaded") {
      REQUIRE(function.declaration.line.has_value());
      overload_lines.insert(*function.declaration.line);
    }
  }
  CHECK(overload_lines == std::set<std::uint64_t>{13, 17});
}

TEST_CASE("every emitted function interval matches an independent ELF symbol") {
  std::ifstream input(reference_path);
  REQUIRE(input.good());
  std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> symbols;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::uint64_t address = 0, size = 0;
    char type = 0;
    std::string name;
    if (fields >> std::hex >> address >> size >> type >> name && (type == 't' || type == 'T')) {
      symbols.emplace(address, std::make_pair(size, name));
    }
  }
  REQUIRE_FALSE(symbols.empty());
  const auto binary = neko::dwarf::inspect(fixture_path);
  std::set<std::uint64_t> identities;
  std::size_t count = 0;
  for (const auto& unit : binary.units) {
    CHECK(unit.unlocated_functions.empty());
    for (const auto& function : unit.functions) {
      CHECK(identities.insert(function.die_offset).second);
      REQUIRE(function.ranges.size() == 1);
      const auto& range = function.ranges.front();
      const auto symbol = symbols.find(range.begin);
      REQUIRE(symbol != symbols.end());
      CHECK(range.end > range.begin);
      CHECK(range.end - range.begin == symbol->second.first);
      if (function.linkage_name) {
        CHECK(*function.linkage_name == symbol->second.second);
      }
      ++count;
    }
  }
  CHECK(count == 10);
}

TEST_CASE("paths with spaces work and inspecting does not change file contents") {
  const scratch_file copy;
  const auto before = fixture_bytes();
  copy.write(before);
  CHECK(neko::dwarf::inspect(copy.path()).units.size() == 3);
  std::ifstream input(copy.path(), std::ios::binary);
  const std::vector<char> after{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  CHECK(before == after);
}

TEST_CASE("missing files and non-regular files are rejected") {
  const scratch_file missing;
  CHECK_THROWS_WITH(neko::dwarf::inspect(missing.path()), doctest::Contains("cannot open binary"));
  CHECK_THROWS_WITH(neko::dwarf::inspect(missing.path().parent_path()),
                    doctest::Contains("regular file"));
}

TEST_CASE("malformed or unsupported ELF headers are rejected") {
  const scratch_file broken;
  auto bytes = fixture_bytes();
  SUBCASE("empty") {
    bytes.clear();
  }
  SUBCASE("truncated") {
    bytes.resize(24);
  }
  SUBCASE("magic") {
    bytes[0] = 0;
  }
  SUBCASE("class") {
    bytes[EI_CLASS] = ELFCLASS32;
  }
  SUBCASE("endian") {
    bytes[EI_DATA] = ELFDATA2MSB;
  }
  SUBCASE("machine") {
    bytes[18] = 0;
  }
  SUBCASE("relocatable") {
    bytes[16] = ET_REL;
  }
  SUBCASE("overflowing program table offset") {
    std::fill(bytes.begin() + 32, bytes.begin() + 40, static_cast<char>(0xff));
  }
  broken.write(bytes);
  CHECK_THROWS_AS(neko::dwarf::inspect(broken.path()), std::runtime_error);
}

TEST_CASE("corrupt embedded DWARF fails without returning an incomplete index") {
  auto bytes = fixture_bytes();
  Elf64_Ehdr header{};
  REQUIRE(bytes.size() >= sizeof(header));
  std::memcpy(&header, bytes.data(), sizeof(header));
  Elf64_Shdr names{};
  const auto names_offset = header.e_shoff + header.e_shstrndx * header.e_shentsize;
  REQUIRE(names_offset + sizeof(names) <= bytes.size());
  std::memcpy(&names, bytes.data() + names_offset, sizeof(names));
  bool corrupted = false;
  for (unsigned i = 0; i < header.e_shnum; ++i) {
    Elf64_Shdr section{};
    const auto offset = header.e_shoff + i * header.e_shentsize;
    REQUIRE(offset + sizeof(section) <= bytes.size());
    std::memcpy(&section, bytes.data() + offset, sizeof(section));
    REQUIRE(names.sh_offset + section.sh_name < bytes.size());
    if (std::strcmp(bytes.data() + names.sh_offset + section.sh_name, ".debug_info") == 0) {
      REQUIRE(section.sh_size >= 12);
      REQUIRE(section.sh_offset + 12 <= bytes.size());
      std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(section.sh_offset), 12,
                  static_cast<char>(0xff));
      corrupted = true;
      break;
    }
  }
  REQUIRE(corrupted);
  const scratch_file broken;
  broken.write(bytes);
  CHECK_THROWS_AS(neko::dwarf::inspect(broken.path()), std::runtime_error);
}

TEST_CASE("repeated inspection and rejection release file descriptors") {
  const auto count_descriptors = [] {
    const auto directory = std::filesystem::exists("/proc/self/fd") ? "/proc/self/fd" : "/dev/fd";
    return std::distance(std::filesystem::directory_iterator(directory),
                         std::filesystem::directory_iterator());
  };
  const scratch_file broken;
  broken.write({});
  const auto before = count_descriptors();
  for (int i = 0; i < 30; ++i) {
    CHECK(neko::dwarf::inspect(fixture_path).units.size() == 3);
    CHECK_THROWS_AS(neko::dwarf::inspect(broken.path()), std::runtime_error);
  }
  CHECK(count_descriptors() == before);
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  fixture_path = argv[1];
  reference_path = argv[2];
  doctest::Context context;
  return context.run();
}
