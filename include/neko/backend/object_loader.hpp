// object_loader — turning a fresh object file into executable code.
//
// Relocation is object-format work requiring symbol context: which symbol
// resolves to old state, new code, or an external trampoline. Keep it separate
// from the executable-memory and entry-patching responsibilities of code_substituter.
//
// Backends: ELF64 relocatable objects (Linux, today), PE/COFF (Windows,
// planned).

#pragma once

#include <neko/backend/code_substituter.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neko::backend {

/// One function that must be redirected after a successful load.
struct function_replacement {
  /// Mangled symbol name, for logging.
  std::string name;
  /// Entry of the old body in the live process.
  std::uintptr_t old_entry = 0;
  /// Offset of the fresh body inside the loaded image.
  std::uint32_t offset_in_image = 0;
};

/// The target category of a symbol participating in generation-wide linking.
/// Object symbols are modeled for upcoming state allocation; the current ELF
/// loader exports and resolves only function symbols.
enum class generation_symbol_kind : std::uint8_t {
  function,
  object,
};

/// A link-visible definition this object contributes to its generation.
struct generation_symbol {
  std::string name;
  generation_symbol_kind kind = generation_symbol_kind::function;
  std::uint32_t offset_in_image = 0;
};

/// The encoding a generation fixup writes. New encodings are added only when
/// a backend implements them; modeling object symbols alone does not claim
/// that object-reference fixups are supported.
enum class generation_fixup_kind : std::uint8_t {
  function_trampoline,
};

/// An unresolved reference whose target may be defined by a sibling image.
struct generation_fixup {
  std::string symbol_name;
  generation_symbol_kind target_kind = generation_symbol_kind::function;
  generation_fixup_kind kind = generation_fixup_kind::function_trampoline;
  std::uint32_t offset_in_image = 0;
};

/// Mutable storage visible from one candidate image. `identity` is the
/// link-level identity used across generations: externally bound names are
/// process-wide, while file-local names include their source identity.
struct state_definition {
  std::string identity;
  std::string name;
  std::uintptr_t address = 0;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
  bool introduced = false;
};

/// A fresh object file placed into executable memory. Local and process
/// references are already relocated; generation-wide references remain as
/// typed fixups until `link_generation()`.
struct loaded_image {
  /// Candidate executable mapping. Preparation owns it here; a successful
  /// transaction moves it into reload_session's active allocation set.
  executable_allocation_ptr allocation;
  /// Function entries that still need redirecting.
  std::vector<function_replacement> replacements;
  /// Link-visible definitions provided to sibling images of the generation.
  std::vector<generation_symbol> exported_symbols;
  /// Typed references deferred until the complete generation is available.
  std::vector<generation_fixup> pending_fixups;
  /// Candidate writable mappings. Rejected candidates reclaim these
  /// automatically; reload_session retains them only after a successful
  /// commit or an incomplete rollback that may have exposed their addresses.
  std::vector<writable_allocation_ptr> state_allocations;
  /// State identities resolved or introduced while loading this image.
  std::vector<state_definition> state_definitions;

  [[nodiscard]] void* code() noexcept {
    return allocation == nullptr ? nullptr : allocation->data();
  }

  [[nodiscard]] const void* code() const noexcept {
    return allocation == nullptr ? nullptr : allocation->data();
  }

  [[nodiscard]] std::uint64_t code_size() const noexcept {
    return allocation == nullptr ? 0 : allocation->size();
  }
};

class object_loader {
public:
  virtual ~object_loader() = default;

  /// Load, relocate and lay out an object file's code. Throws
  /// std::runtime_error with a human-readable reason on failure.
  virtual loaded_image load(const std::uint8_t* object_data, std::size_t size) = 0;

  /// Load an object that the caller identifies as a particular source file.
  /// Backends that do not need translation-unit identity keep working through
  /// the two-argument load() above. Backends that disambiguate file-static
  /// symbols override this overload and consume `source_path`.
  virtual loaded_image load(const std::uint8_t* object_data, std::size_t size,
                            std::string_view source_path) {
    (void)source_path;
    return load(object_data, size);
  }

  /// Resolve typed symbol references between the objects of one candidate
  /// generation. Called once every object is loaded and before validation or
  /// commit; throws when a pending fixup has no compatible sibling
  /// definition. Process-visible references have already been handled by
  /// `load()`. The default accepts images without pending cross links.
  virtual void link_generation(std::span<loaded_image* const> images) { (void)images; }

  /// Perform every potentially-throwing operation needed to publish state
  /// introduced by this generation. Called before the first live entry write.
  virtual void prepare_generation_commit(std::span<loaded_image* const> images) { (void)images; }

  /// Publish state after every entry redirect succeeded. Implementations must
  /// not throw: live code can observe the candidate addresses after this
  /// boundary. Allocation ownership stays in loaded_image and is transferred
  /// into reload_session by the transaction engine.
  virtual void commit_generation(std::span<loaded_image* const> images) noexcept { (void)images; }
};

} // namespace neko::backend
