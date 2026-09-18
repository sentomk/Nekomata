// process_symbols — the live-process symbol table behind the PDB backend.
//
// Answers "where is everything" for the executable hosting the reload
// session through the MS DIA SDK: function entries and extents from the
// PDB's publics and function records, global-variable storage for state
// binding. Names are the decorated bytes the drivers emit (the COFF reader
// and DIA agree on MSVC mangling); lookups are exact and case-sensitive.
//
// File-static functions do not appear in the publics stream — resolving
// them per translation unit needs the compiland walk of the offline symbol
// manifest (planned, mirroring the ELF backend's DWARF manifest); by-name
// lookups for them miss until then, which the loader reports rather than
// guesses.
//
// Threading: a DIA session is single-threaded. reload_session's external
// serialization contract covers every call into this table.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/backend/types.hpp>

namespace neko::pe {

class process_symbols final : public backend::symbol_provider, public backend::state_manager {
public:
  /// Resolves and loads the PDB of the current executable. Throws
  /// std::runtime_error when no DIA source or no usable PDB is available.
  process_symbols();
  ~process_symbols() override;

  process_symbols(const process_symbols&) = delete;
  process_symbols& operator=(const process_symbols&) = delete;

  std::vector<backend::function_info> all_functions() const override;
  std::optional<backend::function_info> function_by_name(std::string_view name) const override;
  std::size_t count_functions(std::string_view name) const override;
  std::optional<backend::global_variable> global_by_name(std::string_view name) const override;
  std::size_t count_globals(std::string_view name) const override;
  backend::type_layout layout_of(backend::type_id id) const override;

  void* map_global(std::string_view name) override;

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace neko::pe
