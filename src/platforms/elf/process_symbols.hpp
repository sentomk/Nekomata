// process_symbols — the live process's own symbol table.
//
// Today this reads the ELF .symtab of /proc/self/exe. This requires the binary
// to be built -no-pie (link-time addresses == runtime addresses) and not
// stripped — both are demo build choices; PIE support is planned
// via load-base detection (dl_iterate_phdr).
//
// Implements both symbol_provider (where are the functions/globals) and
// state_manager (existing storage for a mutable global) since both come from
// the same table. Externals (libc, ...) are resolved through dlsym.

#pragma once

#include <dlfcn.h>

#include <cstdint>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <neko/runtime/state_manager.hpp>
#include <neko/runtime/symbol_provider.hpp>

#include "symbol_manifest.hpp"

namespace neko::elf {

class process_symbols final : public symbol_provider, public state_manager {
public:
  process_symbols();

  std::vector<function_info> all_functions() const override;
  std::optional<function_info> function_by_name(std::string_view name) const override;
  std::size_t count_functions(std::string_view name) const override;
  std::size_t count_globals(std::string_view name) const override;
  std::optional<global_variable> global_by_name(std::string_view name) const override;
  type_layout layout_of(type_id id) const override;

  void* map_global(std::string_view name) override;

  /// Resolve an undefined symbol from a fresh object against the executable
  /// or a shared library. A process-table candidate must have external
  /// binding; a same-named STB_LOCAL symbol belongs to another translation
  /// unit and is never a legal definition for this reference.
  void* resolve_external(std::string_view name, std::uint8_t type, std::uint8_t binding);

  /// Pick the function named `name` that belongs to `source_path`, when the
  /// name alone is not enough to tell.
  ///
  /// Two same-named static functions are ordinary in real code, and the symbol
  /// table cannot say which is which. Two things can: the binding — a global
  /// name is unique by construction, so only a local one is genuinely
  /// ambiguous — and the offline manifest, which records which source file
  /// defined each address. The build integration supplies that source path;
  /// it must not be guessed from a symbol set that changes with every edit.
  ///
  /// Returns nullopt when the evidence is not conclusive; the caller refuses
  /// rather than guesses, since a guess redirects the wrong function.
  [[nodiscard]] std::optional<function_info> function_in_source(std::string_view name,
                                                                std::uint8_t binding,
                                                                std::string_view source_path) const;

  /// Whether the loaded manifest describes `source_path` at all. When it does,
  /// a missing file-static name is a new function, not a license to redirect
  /// the same name from another source.
  [[nodiscard]] bool contains_source(std::string_view source_path) const;

private:
  std::vector<function_info> functions_;
  /// Parallel to functions_: the ELF binding, which is what tells an
  /// unambiguous global name from a genuinely ambiguous local one.
  std::vector<std::uint8_t> function_bindings_;
  /// Symbol-to-source map produced offline; empty when none was found.
  symbol_manifest manifest_;
  /// Load base for a PIE image, so link-time information (the manifest) and
  /// runtime addresses (this table) can be compared.
  std::uintptr_t load_base_ = 0;
  std::vector<global_variable> globals_;
  /// Parallel to globals_: local storage is valid for state preservation in
  /// its own translation unit, but cannot satisfy another unit's undefined
  /// reference.
  std::vector<std::uint8_t> global_bindings_;
  std::unordered_map<std::string, std::vector<std::size_t>> function_index_;
  std::unordered_map<std::string, std::vector<std::size_t>> global_index_;
};

} // namespace neko::elf
