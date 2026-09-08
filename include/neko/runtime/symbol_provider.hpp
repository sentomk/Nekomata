// symbol_provider — symbols and debug information.
//
// Owns the "where is everything" knowledge of the live process: function
// addresses and extents, global variable storage, optimized-build inline units
// reconstructed from DWARF `DW_TAG_inlined_subroutine`.
//
// Backends: ELF .symtab of /proc/self/exe (Linux, today), PDB via MS DIA
// SDK (Windows, planned).
//
// The loader resolves fresh-code symbols against process symbols by mangled name.

#pragma once

#include <neko/core/types.hpp>
#include <neko/runtime/fwd.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace neko {

class symbol_provider {
public:
  virtual ~symbol_provider() = default;

  /// All functions known in the live process.
  virtual std::vector<function_info> all_functions() const = 0;

  /// Look up one function by its mangled symbol name. When several symbols
  /// share the name the result is arbitrary — callers that must not guess
  /// check count_functions() first.
  virtual std::optional<function_info> function_by_name(std::string_view name) const = 0;

  /// How many symbols share this function name (0 = none, 1 = unique).
  /// Matching by name is only safe when this returns 1.
  virtual std::size_t count_functions(std::string_view name) const = 0;

  /// Look up one global/static variable by its symbol name; arbitrary when
  /// ambiguous — see count_globals().
  virtual std::optional<global_variable> global_by_name(std::string_view name) const = 0;

  /// How many symbols share this global name (0 = none, 1 = unique).
  virtual std::size_t count_globals(std::string_view name) const = 0;

  /// Layout of a user-defined type. Not exercised yet; layout migration will
  /// drive its final shape (object layout migration).
  virtual type_layout layout_of(type_id id) const = 0;
};

} // namespace neko
