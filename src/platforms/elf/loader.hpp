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

#include <neko/runtime/fwd.hpp>
#include <neko/runtime/object_loader.hpp>

namespace neko::elf {

class loader final : public object_loader {
public:
  loader(symbol_provider& symbols, state_manager& state, code_substituter& substituter);

  loaded_image load(const std::uint8_t* object_data, std::size_t size) override;

private:
  symbol_provider& symbols_;
  state_manager& state_;
  code_substituter& substituter_;
};

} // namespace neko::elf
