// CodeSubstituter — writing and redirecting machine code in the live process.
//
// Owns the executable-page playground: reserving RWX/RX memory close to the
// target code, applying relocations to freshly compiled code ("runtime
// mini-link"), and rewriting function entry points with a 5-byte `jmp rel32`.
//
// Backends: in-process agent over mmap/mprotect (POSIX), VirtualProtect
// (Windows). Relocation semantics are provided by the ELF / PE backends.

#pragma once

#include <neko/types.hpp>

#include <cstdint>

namespace neko {

class CodeSubstituter {
public:
    virtual ~CodeSubstituter() = default;

    /// Reserve `bytes` of executable memory for a freshly compiled function
    /// body. Returns nullptr on failure.
    virtual void* reserveCode(std::uint64_t bytes) = 0;

    /// Rewrite the entry point of `patch.target` so the next call lands in
    /// `patch.new_code`. Must preserve in-flight executions of the old body
    /// (trampoline or /hotpatch hole).
    virtual bool patchEntry(const Patch& patch) = 0;

    /// Apply one relocation against the live process's real addresses.
    virtual bool relocate(const Relocation& relocation) = 0;
};

} // namespace neko
