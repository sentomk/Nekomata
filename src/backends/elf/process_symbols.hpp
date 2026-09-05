// process_symbols — the live process's own symbol table.
//
// Phase 1 reads the ELF .symtab of /proc/self/exe. This requires the binary
// to be built -no-pie (link-time addresses == runtime addresses) and not
// stripped — both are demo build choices; PIE support arrives in Phase 2
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

namespace neko::elf {

class process_symbols final : public symbol_provider, public state_manager {
public:
  process_symbols();

  std::vector<function_info> all_functions() const override;
  std::optional<function_info> function_by_name(std::string_view name) const override;
  std::optional<global_variable> global_by_name(std::string_view name) const override;
  type_layout layout_of(type_id id) const override;

  void* map_global(std::string_view name) override;

  /// Resolve an external symbol (libc, libstdc++, ...) by name.
  static void* resolve_external(std::string_view name);

private:
  std::vector<function_info> functions_;
  std::vector<global_variable> globals_;
  std::unordered_map<std::string, std::size_t> function_index_;
  std::unordered_map<std::string, std::size_t> global_index_;
};

} // namespace neko::elf
