// Loader tests: a fresh COFF object becomes a real executable image whose
// code runs, with relocations applied against a deterministic fake symbol
// provider — no DIA, no live-process dependence. The fixture calls an
// external the provider resolves (through an in-image trampoline) and one
// only a generation sibling could define (a pending fixup).

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <neko/backend/symbol_provider.hpp>

#include "runtime/code_pages.hpp"
#include "runtime/loader.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using neko::backend::function_info;

// exec.c and relocs.c object files.
std::array<std::filesystem::path, 2> fixtures;

int g_external_calls = 0;

__declspec(noinline) int exec_external_impl(int x) {
  ++g_external_calls;
  return x * 2;
}

// The "old body" the provider reports for the reloaded function; only its
// address matters here — the loader records it, nothing jumps into it.
__declspec(noinline) int exec_plain_old(int x) {
  return x;
}

class fake_symbols : public neko::backend::symbol_provider {
public:
  std::vector<neko::backend::function_info> all_functions() const override { return {}; }

  std::optional<neko::backend::function_info>
  function_by_name(std::string_view name) const override {
    const function_info* hit = find(name);
    return hit == nullptr ? std::optional<function_info>(std::nullopt)
                          : std::optional<function_info>(*hit);
  }

  std::size_t count_functions(std::string_view name) const override {
    return find(name) == nullptr ? 0 : 1;
  }

  std::optional<neko::backend::global_variable>
  global_by_name(std::string_view name) const override {
    (void)name;
    return std::nullopt;
  }

  std::size_t count_globals(std::string_view name) const override {
    (void)name;
    return 0;
  }

  neko::backend::type_layout layout_of(neko::backend::type_id id) const override {
    (void)id;
    return {};
  }

  void add(const std::string& name, std::uintptr_t address, std::size_t size) {
    neko::backend::function_info info;
    info.name = name;
    info.address = address;
    info.size = size;
    functions_.push_back({name, info});
  }

protected:
  const function_info* find(std::string_view name) const {
    for (const auto& entry : functions_) {
      if (entry.second.name == name) {
        return &entry.second;
      }
    }
    return nullptr;
  }

private:
  std::vector<std::pair<std::string, neko::backend::function_info>> functions_;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

TEST_CASE("a loaded image runs and its relocations hold") {
  neko::pe::code_pages pages;
  fake_symbols symbols;
  symbols.add("exec_plain", reinterpret_cast<std::uintptr_t>(&exec_plain_old), 64);
  symbols.add("exec_external", reinterpret_cast<std::uintptr_t>(&exec_external_impl), 64);
  neko::pe::loader load(symbols, pages);

  const auto bytes = read_file(fixtures[0]);
  auto image = load.load(bytes.data(), bytes.size());

  // One redirectable function; the sibling-caller has no live entry.
  REQUIRE(image.replacements.size() == 1);
  CHECK(image.replacements[0].name == "exec_plain");
  CHECK(image.replacements[0].old_entry == reinterpret_cast<std::uintptr_t>(&exec_plain_old));

  // Both external functions are generation-visible.
  bool saw_plain = false;
  bool saw_pending = false;
  for (const auto& exported : image.exported_symbols) {
    saw_plain = saw_plain || exported.name == "exec_plain";
    saw_pending = saw_pending || exported.name == "exec_pending";
  }
  CHECK(saw_plain);
  CHECK(saw_pending);

  // The unresolved sibling left exactly one trampoline fixup.
  const auto* code = static_cast<const std::uint8_t*>(image.code());
  REQUIRE(image.pending_fixups.size() == 1);
  CHECK(image.pending_fixups[0].symbol_name == "exec_sibling");
  CHECK(image.pending_fixups[0].kind == neko::backend::generation_fixup_kind::function_trampoline);
  const auto thunk = image.pending_fixups[0].offset_in_image;
  CHECK(code[thunk] == 0x49); // movabs r11, 0
  CHECK(code[thunk + 2] == 0x00);
  CHECK(code[thunk + 10] == 0x41); // jmp r11

  // Run the fresh body: the call goes through the trampoline to the
  // resolved external, the const load reads placed rodata.
  g_external_calls = 0;
  const auto entry = reinterpret_cast<int (*)(int)>(const_cast<std::uint8_t*>(code) +
                                                    image.replacements[0].offset_in_image);
  CHECK(entry(5) == 13); // 5 * 2 + 3
  CHECK(g_external_calls == 1);
}

TEST_CASE("an object with no live match refuses to load") {
  neko::pe::code_pages pages;
  fake_symbols symbols;
  symbols.add("exec_external", reinterpret_cast<std::uintptr_t>(&exec_external_impl), 64);
  neko::pe::loader load(symbols, pages);

  const auto bytes = read_file(fixtures[0]);
  CHECK_THROWS_WITH(load.load(bytes.data(), bytes.size()), doctest::Contains("nothing to reload"));
}

TEST_CASE("mutable initializers reject until the state path lands") {
  neko::pe::code_pages pages;
  fake_symbols symbols;
  symbols.add("add_probe", 0x1000, 64);
  neko::pe::loader load(symbols, pages);

  const auto bytes = read_file(fixtures[1]);
  CHECK_THROWS_WITH(load.load(bytes.data(), bytes.size()), doctest::Contains("not supported yet"));
}

TEST_CASE("ambiguous live names refuse rather than guess") {
  class ambiguous_symbols final : public fake_symbols {
  public:
    std::size_t count_functions(std::string_view name) const override {
      return name == "exec_plain" ? 2 : fake_symbols::count_functions(name);
    }
  };

  neko::pe::code_pages pages;
  ambiguous_symbols symbols;
  symbols.add("exec_plain", reinterpret_cast<std::uintptr_t>(&exec_plain_old), 64);
  // exec_pending keeps the hint alive so the ambiguity itself is what fires.
  symbols.add("exec_pending", reinterpret_cast<std::uintptr_t>(&exec_plain_old), 64);
  symbols.add("exec_external", reinterpret_cast<std::uintptr_t>(&exec_external_impl), 64);
  neko::pe::loader load(symbols, pages);

  const auto bytes = read_file(fixtures[0]);
  CHECK_THROWS_WITH(load.load(bytes.data(), bytes.size()),
                    doctest::Contains("refusing to patch by name"));
}

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  std::copy_n(argv + 1, fixtures.size(), fixtures.begin());
  doctest::Context context;
  return context.run();
}
