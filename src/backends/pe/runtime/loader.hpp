// loader — the PE backend's runtime mini-link.
//
// Pipeline:
//   1. parse the fresh .obj                    (format/object_file)
//   2. plan the arena image                    (format/image_layout)
//   3. reserve near the code being replaced    (code_substituter)
//   4. copy code and unwind sections, write trampolines, apply relocations
//   5. bind mutable state onto live storage or fresh writable storage
//   6. commit the image, register its unwind tables, record replacements
//   7. link_generation resolves cross-object references when the whole
//      candidate set is loaded
//
// Relocation arithmetic is COFF's S + A − P with the addend A stored at the
// site itself: S is the resolved symbol (image placement + value for
// in-object definitions, live state storage for bound globals, a live
// process address for resolved externals, a trampoline slot for calls), P
// the site's image address. ADDR32NB resolves image-relative (S − image
// base), which the unwind tail's RUNTIME_FUNCTION entries need against the
// one pseudo base.
//
// Unwind tables register with the process as soon as an image commits
// executable — entry code never runs before that — and deregister when the
// allocation is reclaimed; a `release_to_process` allocation keeps its
// tables for the process lifetime, matching its pages.
//
// Current limits (documented, detected and reported loudly):
//   * thread-local sections, `.CRT$` initializers, and the MSVC
//     function-local static guard functions
//   * relocations in data sections (mutable initializers) and in read-only
//     tables (vtables, jump tables)
//   * fresh file-local globals without a source identity
//   * a relative_32 reference to an unresolved non-function external
//     resolves only through a trampoline; a generation sibling defining the
//     name as data rejects at link time, because a jump thunk cannot serve
//     a load

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <neko/backend/object_loader.hpp>

namespace neko::backend {
class code_substituter;
class symbol_provider;
class state_manager;
} // namespace neko::backend

namespace neko::pe {

class loader final : public backend::object_loader {
public:
  loader(backend::symbol_provider& symbols, backend::state_manager& state,
         backend::code_substituter& substituter);

  backend::loaded_image load(const std::uint8_t* object_data, std::size_t size) override;
  backend::loaded_image load(const std::uint8_t* object_data, std::size_t size,
                             std::string_view source_path) override;
  void link_generation(std::span<backend::loaded_image* const> images) override;
  void prepare_generation_commit(std::span<backend::loaded_image* const> images) override;
  void commit_generation(std::span<backend::loaded_image* const> images) noexcept override;

private:
  struct committed_state {
    std::string identity;
    std::string name;
    std::uintptr_t address = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
  };

  [[nodiscard]] const committed_state* find_committed_state(std::string_view identity) const;

  backend::symbol_provider& symbols_;
  backend::state_manager& state_;
  backend::code_substituter& substituter_;
  /// Addresses only; the session owns the matching writable allocations.
  /// Publication happens at the commit boundary, after entry patching.
  std::vector<committed_state> committed_state_;
};

} // namespace neko::pe
