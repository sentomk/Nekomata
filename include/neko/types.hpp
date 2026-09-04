// Common types shared by the neko kernel interfaces.
//
// Phase 1 note: these shapes are no longer pure drafts — they have survived
// one real end-to-end implementation (the Linux/ELF prototype). What changed
// and why is recorded in docs/phase-1-notes.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko {

/// Opaque handle for a user-defined type known to the symbol backend.
/// Phase 5 territory (class layout migration); Phase 1 does not exercise it.
using type_id = std::uint32_t;

/// Address and extent of a function inside the live process.
struct function_info {
    /// Mangled symbol name as emitted by the compiler (e.g. `_Z4tickv`).
    std::string name;
    /// Runtime address of the function entry.
    std::uintptr_t address = 0;
    /// Size of the function body in bytes (0 if unknown).
    std::size_t size = 0;
};

/// Address and extent of a global/static variable inside the live process.
struct global_variable {
    std::string name;
    std::uintptr_t address = 0;
    std::size_t size = 0;
};

/// Physical layout of a type. Phase 5 (class layout migration) will extend
/// this with member offsets and vtable information; keep it minimal for now.
struct type_layout {
    type_id id = 0;
    std::size_t size = 0;
    std::size_t alignment = 0;
};

/// Set of source files changed since the last reload.
struct change_set {
    std::vector<std::string> changed_files;
};

/// The outcome of planning: what to recompile before a reload can be applied.
struct patch_plan {
    /// Translation units that must be recompiled.
    std::vector<std::string> translation_units;
};

} // namespace neko
