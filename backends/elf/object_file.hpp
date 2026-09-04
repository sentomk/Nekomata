// Minimal ELF64 little-endian relocatable (.o) reader.
//
// Just enough for the Phase 1 prototype (proposal §阶段一): section
// classification, the symbol table and relocations. Deliberately not a
// general ELF parser — it rejects everything that is not an x86-64
// relocatable object, and ignores anything it does not need (debug info,
// .eh_frame, .comment, ...). ET_EXEC of the running process is handled by
// process_symbols instead.

#pragma once

#include <cstddef>
#include <cstdint>

#include <string>
#include <vector>

namespace neko::elf {

enum class section_class : std::uint8_t {
    text,   // SHF_ALLOC | SHF_EXECINSTR — code, goes into the executable arena
    rodata, // SHF_ALLOC, read-only — string literals etc., arena too
    data,   // SHF_ALLOC | SHF_WRITE — .data/.bss; NOT loaded: existing state
            // wins (proposal: 全局变量按既有地址重定位以保状态)
    other,  // everything we do not load
};

struct section {
    std::string name;
    section_class cls = section_class::other;
    std::uint16_t index = 0;
    std::uint64_t size = 0;
    /// Raw bytes for PROGBITS sections; empty for SHT_NOBITS (.bss).
    std::vector<std::uint8_t> bytes;
};

struct symbol {
    std::string name;                // mangled for C++ entities
    std::uint16_t section_index = 0; // 0 = SHN_UNDEF
    std::uint8_t type = 0;           // ELF st_type (STT_*)
    std::uint8_t bind = 0;           // ELF st_bind (STB_*)
    /// Offset within its section (ET_REL semantics).
    std::uint64_t value = 0;
    std::uint64_t size = 0;
};

struct relocation {
    std::uint16_t target_section = 0; // section the relocation applies to
    std::uint64_t offset = 0;         // offset within that section
    std::uint32_t symbol_index = 0;
    std::uint32_t type = 0; // R_X86_64_*
    std::int64_t addend = 0;
};

struct object_file {
    std::vector<section> sections;
    std::vector<symbol> symbols;
    std::vector<relocation> relocations;
};

/// Parse an ELF64 LSB relocatable object. Throws std::runtime_error with a
/// human-readable reason on anything unexpected.
object_file parse_object(const std::uint8_t* data, std::size_t size);

} // namespace neko::elf
