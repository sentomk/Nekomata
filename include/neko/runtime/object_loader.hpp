// object_loader — turning a fresh object file into executable code.
//
// Relocation is object-format work requiring symbol context: which symbol
// resolves to old state, new code, or an external trampoline. Keep it separate
// from the executable-memory and entry-patching responsibilities of code_substituter.
//
// Backends: ELF64 relocatable objects (Linux, today), PE/COFF (Windows,
// planned).

#pragma once

#include <neko/runtime/fwd.hpp>

#include <cstddef>
#include <cstdint>

#include <string>
#include <string_view>
#include <vector>

namespace neko {

/// One function that must be redirected after a successful load.
struct function_replacement {
  /// Mangled symbol name, for logging.
  std::string name;
  /// Entry of the old body in the live process.
  std::uintptr_t old_entry = 0;
  /// Offset of the fresh body inside the loaded image.
  std::uint32_t offset_in_image = 0;
};

/// A fresh object file placed into executable memory, fully relocated.
struct loaded_image {
  /// Executable mapping owned by the code_substituter.
  void* code = nullptr;
  std::uint64_t code_size = 0;
  /// Function entries that still need redirecting.
  std::vector<function_replacement> replacements;
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
};

} // namespace neko
