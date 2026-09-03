// Common types shared by the nekomata kernel interfaces.
//
// NOTE(design): these shapes are drafts. Phase 1 (single-function replacement
// prototype on Linux/ELF) exists precisely to validate them; expect churn.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neko {

/// Opaque handle for a user-defined type known to the symbol backend.
using type_id = std::uint32_t;

/// Opaque handle for a global/static variable tracked by the state manager.
using global_id = std::uint32_t;

/// Address and extent of a function inside the live process.
struct function_info {
    /// Mangled symbol name as emitted by the compiler (e.g. `_Z4funcv`).
    std::string name;
    /// Runtime address of the function entry, as loaded in the target process.
    std::uintptr_t address = 0;
    /// Size of the function body in bytes (0 if unknown).
    std::size_t size = 0;
};

/// Physical layout of a type. Phase 5 (class layout migration) will extend
/// this with member offsets and vtable information; keep it minimal for now.
struct type_layout {
    type_id id = 0;
    std::size_t size = 0;
    std::size_t alignment = 0;
};

/// One relocation that must be applied to freshly compiled code before it can
/// run at its new address. Applying all relocations of an object file is the
/// "runtime mini-link" step of the reload pipeline.
struct Relocation {
    /// Offset of the relocation target inside the new code buffer.
    std::uint64_t offset = 0;
    /// Symbol the relocation refers to.
    std::string symbol;
    /// Addend carried by the relocation, if any.
    std::int64_t addend = 0;
    /// Backend-specific relocation type (e.g. `R_X86_64_PC32` for ELF).
    /// TODO(phase-1): consider a tagged union / variant per backend instead of
    /// a raw number.
    std::uint32_t type = 0;
};

/// A single entry-point rewrite: redirect calls to `target` at `new_code`.
///
/// On x86-64 the entry rewrite is a `jmp rel32` (5 bytes) — either into a
/// `/hotpatch`-reserved hole or over a trampoline copied from the prologue.
struct Patch {
    /// The function being replaced.
    function_info target;
    /// Buffer produced by code_substituter::reserve_code() holding the new body.
    void* new_code = nullptr;
    /// Relocations that must be applied to `new_code` before patching.
    std::vector<Relocation> relocations;
};

/// Set of source files changed since the last reload.
struct change_set {
    std::vector<std::string> changed_files;
};

/// The outcome of planning: what to recompile and what to replace afterwards.
struct patch_plan {
    /// Translation units that must be recompiled.
    std::vector<std::string> translation_units;
    /// Function patches to apply once recompilation succeeded.
    std::vector<Patch> patches;
};

} // namespace neko
