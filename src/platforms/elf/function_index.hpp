#pragma once

#include "../dwarf/inspect.hpp"
#include "binary_file.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace neko::elf {

enum class match_status {
  matched,
  missing_symtab,
  missing_symbol,
  ambiguous,
  unknown_size,
  range_mismatch,
  linkage_mismatch,
  unsupported_ranges
};

const char* status_name(match_status status);

struct function_match {
  // Indices into debug.units[unit].functions[function] and symbols.functions.
  std::size_t unit = 0;
  std::size_t function = 0;
  match_status status = match_status::missing_symbol;
  // Every ELF function at this entry address, including aliases. A linkage
  // name never hides additional candidates. No selected symbol unless matched.
  std::vector<std::size_t> candidates;
};

struct function_index {
  dwarf::binary_info debug;
  function_symbols symbols;
  std::vector<function_match> functions;
  // Includes startup/library code without DWARF. Not automatically an error.
  std::vector<std::size_t> unassociated_symbols;
};

// Pure association of already validated records FROM THE SAME BINARY.
// Only one exact, nonzero range and (when present) matching linkage name can
// yield matched. Ambiguities are data, malformed file input is an exception.
// This is not a replacement plan or a cross-build identity/matching API.
function_index associate(dwarf::binary_info debug, function_symbols symbols);
function_index inspect_functions(const std::filesystem::path& path);

} // namespace neko::elf
