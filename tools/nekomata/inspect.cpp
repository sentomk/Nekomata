#include "inspect.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

int inspect_binary(const char* path) {
  try {
    // Validate completely before printing a possibly incomplete index.
    const auto binary = neko::dwarf::inspect(path);
    std::cout << "binary " << std::quoted(binary.path.string()) << '\n'
              << "addresses: link-time virtual addresses; ranges: [begin, end)\n";
    std::size_t functions = 0;
    std::size_t unlocated = 0;
    for (const auto& unit : binary.units) {
      std::cout << "cu die=0x" << std::hex << unit.die_offset << std::dec
                << " name=" << std::quoted(unit.name)
                << " directory=" << std::quoted(unit.directory)
                << " producer=" << std::quoted(unit.producer) << '\n';
      for (const auto& function : unit.functions) {
        std::cout << "  function die=0x" << std::hex << function.die_offset << std::dec
                  << " name=" << std::quoted(function.name) << " linkage=";
        if (function.linkage_name) {
          std::cout << std::quoted(*function.linkage_name);
        } else {
          std::cout << "<unavailable>";
        }
        for (const auto& range : function.ranges) {
          std::cout << " [0x" << std::hex << range.begin << ", 0x" << range.end << ')' << std::dec;
        }
        std::cout << '\n';
        ++functions;
      }
      for (const auto& name : unit.unlocated_functions) {
        std::cout << "  no-emitted-range name=" << std::quoted(name) << '\n';
        ++unlocated;
      }
    }
    std::cout << "summary: " << binary.units.size() << " compilation unit(s), " << functions
              << " function(s) with code, " << unlocated << " without emitted ranges\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "inspect failed: " << error.what() << '\n';
    return 1;
  }
}
