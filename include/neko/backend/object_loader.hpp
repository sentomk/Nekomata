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

/// A link-visible function this object defines in its fresh image. Cross-TU
/// calls that cannot resolve against the process are patched to these.
struct exported_function {
  std::string name;
  std::uint32_t offset_in_image = 0;
};

/// A call this object could not resolve at load time; the trampoline bytes
/// carry a placeholder until `link_generation()` patches them or rejects.
struct pending_call_fixup {
  std::string name;
  std::uint32_t trampoline_offset_in_image = 0;
};

/// A fresh object file placed into executable memory, fully relocated.
struct loaded_image {
  /// Candidate executable mapping. Preparation owns it here; a successful
  /// transaction moves it into reload_session's active allocation set.
  executable_allocation_ptr allocation;
  /// Function entries that still need redirecting.
  std::vector<function_replacement> replacements;
  /// Link-visible functions provided to sibling objects of the generation.
  std::vector<exported_function> exported_functions;
  /// Calls deferred to `link_generation()` — new cross-TU symbols.
  std::vector<pending_call_fixup> pending_call_fixups;

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

  /// Resolve calls between the objects of one candidate generation. Called
  /// once every object of the generation is loaded and before validation or
  /// commit; throws for a call no sibling defines and the process cannot
  /// resolve either. The default accepts images without pending cross links.
  virtual void link_generation(std::span<loaded_image* const> images) { (void)images; }
};

} // namespace neko::backend
