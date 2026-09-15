#include "function_index.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace neko::elf {

const char* status_name(match_status status) {
  switch (status) {
  case match_status::matched:
    return "matched";
  case match_status::missing_symtab:
    return "missing-symtab";
  case match_status::missing_symbol:
    return "missing-symbol";
  case match_status::ambiguous:
    return "ambiguous";
  case match_status::unknown_size:
    return "unknown-size";
  case match_status::range_mismatch:
    return "range-mismatch";
  case match_status::linkage_mismatch:
    return "linkage-mismatch";
  case match_status::unsupported_ranges:
    return "unsupported-ranges";
  }
  return "invalid-status";
}

function_index associate(dwarf::binary_info debug, function_symbols symbols) {
  if (symbols.kind == binary_kind::relocatable) {
    throw std::runtime_error("relocatable association requires section-aware DWARF ranges");
  }
  function_index out{std::move(debug), std::move(symbols), {}, {}};
  std::map<std::uint64_t, std::vector<std::size_t>> by_address;
  for (std::size_t i = 0; i < out.symbols.functions.size(); ++i) {
    by_address[out.symbols.functions[i].address].push_back(i);
  }
  std::vector<bool> associated(out.symbols.functions.size(), false);
  constexpr std::size_t max_candidates = 1000000;
  std::size_t candidate_count = 0;
  // Multiple DWARF definitions at one entry are also ambiguous, even if ELF
  // has only one symbol there (e.g. linker folding or duplicate debug records).
  std::map<std::uint64_t, std::size_t> definition_count;
  for (const auto& unit : out.debug.units) {
    for (const auto& function : unit.functions) {
      if (function.ranges.size() == 1) {
        ++definition_count[function.ranges.front().begin];
      }
    }
  }
  for (std::size_t u = 0; u < out.debug.units.size(); ++u) {
    const auto& unit = out.debug.units[u];
    for (std::size_t f = 0; f < unit.functions.size(); ++f) {
      const auto& function = unit.functions[f];
      function_match match{u, f, match_status::missing_symbol, {}};
      if (!out.symbols.has_symtab) {
        match.status = match_status::missing_symtab;
      } else if (function.ranges.size() != 1 ||
                 function.ranges.front().end <= function.ranges.front().begin) {
        match.status = match_status::unsupported_ranges;
      } else {
        const auto& range = function.ranges.front();
        if (const auto found = by_address.find(range.begin); found != by_address.end()) {
          // Folded definitions and aliases can otherwise multiply N records by
          // M symbols. Bound the complete result before copying each group.
          if (found->second.size() > max_candidates - candidate_count) {
            throw std::runtime_error("function association exceeds candidate limit");
          }
          candidate_count += found->second.size();
          match.candidates = found->second;
          for (const auto candidate : match.candidates) {
            associated[candidate] = true;
          }
          if (match.candidates.size() != 1 || definition_count[range.begin] != 1) {
            match.status = match_status::ambiguous;
          } else {
            const auto& symbol = out.symbols.functions[match.candidates.front()];
            if (symbol.size == 0) {
              match.status = match_status::unknown_size;
            } else if (symbol.size != range.end - range.begin) {
              match.status = match_status::range_mismatch;
            } else if (function.linkage_name && *function.linkage_name != symbol.name) {
              match.status = match_status::linkage_mismatch;
            } else {
              match.status = match_status::matched;
            }
          }
        }
      }
      out.functions.push_back(std::move(match));
    }
  }
  for (std::size_t i = 0; i < associated.size(); ++i) {
    if (!associated[i]) {
      out.unassociated_symbols.push_back(i);
    }
  }
  return out;
}

function_index inspect_functions(const std::filesystem::path& path) {
  const binary_file file(path);
  auto symbols = file.symbols();
  auto debug = dwarf::inspect(file);
  return associate(std::move(debug), std::move(symbols));
}

} // namespace neko::elf
