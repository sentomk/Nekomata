// object_loader — turning a fresh object file into executable code.
//
// This interface did not exist in the proposal's draft: the draft folded
// "apply relocations" into code_substituter, but relocation is inherently
// object-format work requiring symbol context (which symbol resolves to old
// state, which to new code, which to an external trampoline). Phase 1 split
// it out; see docs/phase-1-notes.md.
//
// Backends: ELF64 relocatable objects (Linux, Phase 1), PE/COFF (Windows,
// Phase 3).

#pragma once

#include <cstdint>

#include <string>
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
};

} // namespace neko
