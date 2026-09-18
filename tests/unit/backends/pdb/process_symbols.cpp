// DIA process-symbol tests run against this executable's own PDB — the
// same path a reload session's host takes. Address equality with the
// running process is the assertion: the PDB's publics must land exactly on
// the functions and globals this TU defines.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "process_symbols.hpp"

#include <cstdint>
#include <string>
#include <vector>

extern "C" __declspec(noinline) int pdb_probe_function(int x) {
  volatile int sink = x;
  return sink + 1;
}

extern "C" __declspec(noinline) int pdb_probe_sibling(int x) { // same name prefix
  volatile int sink = x;
  return sink + 2;
}

int pdb_global_five = 5;
int pdb_global_answer = 42;

TEST_CASE("the own PDB resolves functions to their live addresses") {
  neko::pe::process_symbols symbols;

  const auto probe = symbols.function_by_name("pdb_probe_function");
  REQUIRE(probe.has_value());
  CHECK(probe->address == reinterpret_cast<std::uintptr_t>(&pdb_probe_function));
  CHECK(probe->size >= 1);

  // The prefixed sibling is a different symbol, not a hit.
  CHECK(symbols.count_functions("pdb_probe_function") == 1);
  CHECK(symbols.function_by_name("pdb_probe_function_missing").has_value() == false);
}

TEST_CASE("globals resolve to their storage for state binding") {
  neko::pe::process_symbols symbols;

  const auto global = symbols.global_by_name("pdb_global_five");
  REQUIRE(global.has_value());
  CHECK(global->address == reinterpret_cast<std::uintptr_t>(&pdb_global_five));
  // Extents may be absent or alignment-padded here; state binding takes the
  // real size from the fresh object's definition, not the live symbol.
  CHECK(*static_cast<int*>(symbols.map_global("pdb_global_five")) == 5);

  CHECK(symbols.count_globals("pdb_global_five") == 1);
  CHECK(symbols.map_global("pdb_global_missing") == nullptr);
}

TEST_CASE("all_functions reports this TU's functions") {
  neko::pe::process_symbols symbols;

  const auto functions = symbols.all_functions();
  REQUIRE(functions.size() > 0);
  bool saw_probe = false;
  for (const auto& function : functions) {
    CHECK(function.address != 0);
    if (function.name == "pdb_probe_function") {
      saw_probe = true;
      CHECK(function.address == reinterpret_cast<std::uintptr_t>(&pdb_probe_function));
    }
  }
  CHECK(saw_probe);
}

TEST_CASE("a fresh table answers consistently") {
  neko::pe::process_symbols first;
  neko::pe::process_symbols second;
  CHECK(first.function_by_name("pdb_probe_sibling")->address ==
        second.function_by_name("pdb_probe_sibling")->address);
}
