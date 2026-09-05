// symbol_provider — symbols and debug information.
//
// Owns the "where is everything" knowledge of the live process: function
// addresses and extents, global variable storage, (Phase 4) inline units
// reconstructed from DWARF `DW_TAG_inlined_subroutine`.
//
// Backends: ELF .symtab of /proc/self/exe (Linux, Phase 1), PDB via MS DIA
// SDK (Windows, Phase 3).
//
// The loader resolves fresh-code symbols against process symbols by mangled name.

#pragma once

#include <neko/core/types.hpp>
#include <neko/runtime/fwd.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace neko {

class symbol_provider {
public:
  virtual ~symbol_provider() = default;

  /// All functions known in the live process.
  virtual std::vector<function_info> all_functions() const = 0;

  /// Look up one function by its mangled symbol name.
  virtual std::optional<function_info> function_by_name(std::string_view name) const = 0;

  /// Look up one global/static variable by its symbol name.
  virtual std::optional<global_variable> global_by_name(std::string_view name) const = 0;

  /// Layout of a user-defined type. Not exercised by Phase 1; Phase 5 will
  /// drive its final shape (object layout migration).
  virtual type_layout layout_of(type_id id) const = 0;
};

} // namespace neko
