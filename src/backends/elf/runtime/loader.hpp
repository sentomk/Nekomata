// loader — the ELF backend's "runtime mini-link" .
//
// Pipeline:
//   1. parse the fresh .o                    (object_file)
//   2. lay out its text/rodata into an arena (near the old code, for rel32)
//   3. allocate or resolve mutable state:
//        mutable data  -> EXISTING process storage  (state preservation)
//        new data      -> owned near-code writable storage
//        text/rodata   -> arena
//        externals     -> in-arena trampolines (movabs+jmp), because libc is
//                         far outside ±2 GiB and `call rel32` cannot reach
//   4. apply .rela.* relocations of text sections into the image
//   5. hand the image to code_substituter for commit + entry patching
//
// Current limits (documented, detected and reported loudly):
//   * thread-local, dynamically initialized, and non-trivial mutable state
//   * relocations in data sections (including pointer initializers)
//   * R_X86_64_GOT-style relocations (the demo TU is built -fno-pic)

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <neko/backend/object_loader.hpp>
#include <neko/fwd.hpp>

#include "process_symbols.hpp"

namespace neko::elf {

using neko::backend::code_substituter;
using neko::backend::function_replacement;
using neko::backend::generation_fixup;
using neko::backend::generation_fixup_kind;
using neko::backend::generation_symbol;
using neko::backend::generation_symbol_kind;
using neko::backend::loaded_image;
using neko::backend::object_loader;
using neko::backend::symbol_provider;

class loader final : public object_loader {
public:
  loader(process_symbols& symbols, state_manager& state, code_substituter& substituter);

  loaded_image load(const std::uint8_t* object_data, std::size_t size) override;
  loaded_image load(const std::uint8_t* object_data, std::size_t size,
                    std::string_view source_path) override;
  void link_generation(std::span<loaded_image* const> images) override;
  void prepare_generation_commit(std::span<loaded_image* const> images) override;
  void commit_generation(std::span<loaded_image* const> images) noexcept override;

private:
  struct committed_state {
    std::string identity;
    std::string name;
    std::uintptr_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
  };

  [[nodiscard]] const committed_state* find_committed_state(std::string_view identity) const;

  process_symbols& symbols_;
  state_manager& state_;
  code_substituter& substituter_;
  /// Addresses only; reload_session owns the matching writable allocations.
  /// Publication happens after entry patching and is prepared before it.
  std::vector<committed_state> committed_state_;
};

} // namespace neko::elf
