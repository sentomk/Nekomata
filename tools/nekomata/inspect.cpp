#include "function_index.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void print_symbol(const neko::elf::function_symbol& symbol, const char* prefix) {
  std::cout << prefix << "symbol table=" << symbol.table_section << " index=" << symbol.table_index
            << " section=" << symbol.section << " name=" << std::quoted(symbol.name)
            << " binding=" << static_cast<unsigned>(symbol.binding)
            << " visibility=" << static_cast<unsigned>(symbol.visibility) << " address=0x"
            << std::hex << symbol.address << " size=0x" << symbol.size << std::dec << '\n';
}

} // namespace

int inspect_binary(const char* path) {
  try {
    // Validate completely before printing a possibly incomplete index.
    const auto index = neko::elf::inspect_functions(path);
    const auto& binary = index.debug;
    std::cout << "binary " << std::quoted(binary.path.string()) << '\n'
              << "addresses: link-time virtual addresses; ranges: [begin, end)\n";
    std::size_t functions = 0;
    std::size_t unlocated = 0;
    std::size_t matched = 0;
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
        const auto& association = index.functions[functions];
        std::cout << " match=" << neko::elf::status_name(association.status) << '\n';
        if (association.status == neko::elf::match_status::matched) {
          ++matched;
        }
        for (const auto candidate : association.candidates) {
          print_symbol(index.symbols.functions[candidate], "    candidate ");
        }
        ++functions;
      }
      for (const auto& name : unit.unlocated_functions) {
        std::cout << "  no-emitted-range name=" << std::quoted(name) << '\n';
        ++unlocated;
      }
    }
    std::cout << "summary: " << binary.units.size() << " compilation unit(s), " << functions
              << " function(s) with code, " << unlocated << " without emitted ranges\n";
    std::cout << "association: " << matched << " matched, " << functions - matched
              << " unresolved; symtab=" << (index.symbols.has_symtab ? "present" : "missing")
              << "; " << index.unassociated_symbols.size()
              << " ELF function(s) without a DWARF entry\n";
    for (const auto symbol : index.unassociated_symbols) {
      print_symbol(index.symbols.functions[symbol], "  unassociated ");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "inspect failed: " << error.what() << '\n';
    return 1;
  }
}
