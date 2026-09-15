// loader — the ELF backend's "runtime mini-link" .
//
// Pipeline:
//   1. parse the fresh .o                    (object_file)
//   2. lay out its text/rodata into an arena (near the old code, for rel32)
//   3. resolve symbols:
//        mutable data  -> EXISTING process storage  (state preservation)
//        text/rodata   -> arena
//        externals     -> in-arena trampolines (movabs+jmp), because libc is
//                         far outside ±2 GiB and `call rel32` cannot reach
//   4. apply .rela.* relocations of text sections into the image
//   5. hand the image to code_substituter for commit + entry patching
//
// Current limits (documented, detected and reported loudly):
//   * new mutable globals in fresh code (no existing storage to map onto)
//   * relocations in data sections (pointer initializers)
//   * R_X86_64_GOT-style relocations (the demo TU is built -fno-pic)

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <neko/backend/object_loader.hpp>
#include <neko/fwd.hpp>

#include "process_symbols.hpp"

namespace neko::elf {

using neko::backend::code_substituter;
using neko::backend::exported_function;
using neko::backend::function_replacement;
using neko::backend::loaded_image;
using neko::backend::object_loader;
using neko::backend::pending_call_fixup;
using neko::backend::symbol_provider;

class loader final : public object_loader {
public:
  loader(process_symbols& symbols, state_manager& state, code_substituter& substituter);

  loaded_image load(const std::uint8_t* object_data, std::size_t size) override;
  loaded_image load(const std::uint8_t* object_data, std::size_t size,
                    std::string_view source_path) override;
  void link_generation(std::span<loaded_image* const> images) override;

private:
  process_symbols& symbols_;
  state_manager& state_;
  code_substituter& substituter_;
};

} // namespace neko::elf
