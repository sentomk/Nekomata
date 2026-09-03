// symbol_provider — symbols and debug information.
//
// Owns the "where is everything" knowledge of the live process: function
// addresses and extents, type layouts, (Phase 4) inline units reconstructed
// from DWARF `DW_TAG_inlined_subroutine`.
//
// Backends: DWARF + ELF .symtab (Linux), PDB via MS DIA SDK (Windows).

#pragma once

#include <neko/types.hpp>

#include <vector>

namespace neko {

class symbol_provider {
public:
    virtual ~symbol_provider() = default;

    /// All functions known in the live process.
    virtual std::vector<function_info> all_functions() const = 0;

    /// Layout of a user-defined type.
    virtual type_layout layout_of(type_id id) const = 0;
};

} // namespace neko
