// loader — the PE backend's runtime mini-link.
//
// Pipeline (single-image scope; the state path lands next):
//   1. parse the fresh .obj                    (format/object_file)
//   2. plan the arena image                    (format/image_layout)
//   3. reserve near the code being replaced    (code_substituter)
//   4. copy code and unwind sections, write trampolines, apply relocations
//   5. commit the image and record replacements
//
// Relocation arithmetic is COFF's S + A − P with the addend A stored at the
// site itself: S is the resolved symbol (image placement + value for
// in-object definitions, a live process address for resolved externals, a
// trampoline slot for calls), P the site's image address. ADDR32NB resolves
// image-relative (S − image base), which is what the unwind tail's
// RUNTIME_FUNCTION entries need against the one pseudo base.
//
// Current limits (documented, detected and reported loudly):
//   * every form of mutable state — existing globals, new globals, common
//     symbols — until the state path lands
//   * relocations in data sections (mutable initializers) and in read-only
//     tables (vtables, jump tables, .CRT$ initializers)
//   * an unresolved relative_32 reference reserves a trampoline slot
//     uniformly — a generation sibling that defines the name as data
//     rejects at link time, because a jump thunk cannot serve a load

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <neko/backend/object_loader.hpp>

namespace neko::backend {
class code_substituter;
class symbol_provider;
} // namespace neko::backend

namespace neko::pe {

class loader final : public backend::object_loader {
public:
  loader(backend::symbol_provider& symbols, backend::code_substituter& substituter);

  backend::loaded_image load(const std::uint8_t* object_data, std::size_t size) override;
  backend::loaded_image load(const std::uint8_t* object_data, std::size_t size,
                             std::string_view source_path) override;

private:
  backend::symbol_provider& symbols_;
  backend::code_substituter& substituter_;
};

} // namespace neko::pe
