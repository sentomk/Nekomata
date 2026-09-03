// SymbolProvider — symbols and debug information.
//
// Owns the "where is everything" knowledge of the live process: function
// addresses and extents, type layouts, (Phase 4) inline units reconstructed
// from DWARF `DW_TAG_inlined_subroutine`.
//
// Backends: DWARF + ELF .symtab (Linux), PDB via MS DIA SDK (Windows).

#pragma once

#include <hlr/types.hpp>

#include <vector>

namespace hlr {

class SymbolProvider {
public:
    virtual ~SymbolProvider() = default;

    /// All functions known in the live process.
    virtual std::vector<FunctionInfo> allFunctions() const = 0;

    /// Layout of a user-defined type.
    virtual TypeLayout layoutOf(TypeId id) const = 0;
};

} // namespace hlr
