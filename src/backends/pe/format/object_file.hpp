// object_file — minimal COFF x86-64 object (.obj) reader for the PE backend.
//
// Just enough for hot reload: section classification, alignment, the raw
// bytes of the sections the loader places in its arena, the symbol table,
// decoded relocations, and the unwind-table association. Deliberately not a
// general COFF parser — it rejects everything that is not an x86-64 object
// from a supported driver family (MSVC and clang-cl), and ignores what it
// does not need. Debug streams stay classified as `other`; unwind sections
// never enter the arena but their association is decoded into
// `unwind_table`.
//
// Surveyed shapes (2026-09-18, both drivers, /Od with and without /Gy): the
// section index is the identity — duplicate names such as `.text` or
// `.text$mn` are normal; COMDAT is unavoidable (inline functions, string
// literals, /Gy); classification follows the IMAGE_SCN_* flags just as the
// ELF reader classifies by sh_flags, with name-based exceptions for tables
// that must never enter the arena. MSVC models uninitialized globals as
// common symbols (undefined section, value = size) where clang defines them
// in .bss; both shapes are preserved below.

#pragma once

#include <cstddef>
#include <cstdint>

#include <string>
#include <vector>

namespace neko::pe {

enum class section_class : std::uint8_t {
  text,   // IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE — code, arena-bound
  rodata, // initialized, read-only — string literals etc., arena too
  data,   // initialized writable plus uninitialized storage; existing symbols
          // bind to live state, supported new symbols receive persistent
          // storage
  other,  // everything we do not load
};

/// How a COMDAT section folds with its duplicates; the selection and the
/// association live in the auxiliary record of the section symbol.
enum class comdat_selection : std::uint8_t {
  none = 0,
  no_duplicates = 1,
  any = 2,
  same_size = 3,
  exact_match = 4,
  associative = 5,
  largest = 6,
};

struct section {
  std::string name;
  section_class cls = section_class::other;
  std::uint16_t index = 0;
  std::uint64_t size = 0;
  /// Decoded from the section flags (a power of two, normalized to >= 1);
  /// the loader must honor it when placing the section in the arena — an
  /// under-aligned constant faults the first aligned SIMD load.
  std::uint64_t align = 1;
  /// Raw bytes for initialized loadable sections; empty for uninitialized
  /// storage and for `other` sections.
  std::vector<std::uint8_t> bytes;
  /// IMAGE_SCN_LNK_COMDAT: the section folds with its duplicates from other
  /// objects.
  bool comdat = false;
  comdat_selection selection = comdat_selection::none;
  /// One-based index of the leader section, meaningful for associative
  /// companions; clang also fills it for leaders (the section's own number),
  /// MSVC leaves zero.
  std::uint16_t association = 0;
};

/// One COFF symbol record. The parser reports what is there — marker
/// symbols (@feat.00 and friends), file and label records pass through —
/// because filtering is loader policy. Names arrive unmangled bytes; MSVC
/// and clang-cl agree on the MSVC mangling, which differs from Itanium's.
struct symbol {
  std::string name;
  /// Raw COFF semantics: 0 undefined, -1 absolute, -2 debug, >= 1 defined in
  /// that section (one-based; `sections[number - 1]`).
  std::int16_t section_number = 0;
  /// The whole Type field; both drivers mark functions with 0x20.
  std::uint16_t type = 0;
  /// IMAGE_SYM_CLASS_*: 2 external, 3 static, 6 label, 103 file.
  std::uint8_t storage_class = 0;
  /// Offset within the section for definitions; the storage size for common
  /// symbols; zero for plain undefined references.
  std::uint32_t value = 0;
  /// Auxiliary records occupy their table slots so that a symbol's vector
  /// index equals its COFF symbol index (relocations reference these).
  /// Their meaning belongs to the preceding symbol and is captured where it
  /// matters (section selections land on the section).
  bool auxiliary = false;

  [[nodiscard]] bool is_function() const { return type == 0x20; }
  /// MSVC's shape for uninitialized globals: external, undefined, value
  /// holding the storage size. clang defines the same variable in .bss.
  [[nodiscard]] bool is_common() const {
    return storage_class == 2 && section_number == 0 && value != 0;
  }
};

/// Decoded IMAGE_REL_AMD64_* categories. The stored field at each site is
/// the addend — the target's offset within its defining section — which the
/// loader combines with the resolved symbol. Encodings that only occur in
/// streams the loader never applies are decoded as `unsupported` and their
/// rejection is loader policy.
enum class relocation_kind : std::uint8_t {
  absolute_64,         // ADDR64 — the target's 64-bit address
  absolute_32,         // ADDR32 — the target's 32-bit zero-extended address
  image_relative_32,   // ADDR32NB — image-base-relative; in an object this
                       // names the symbol's section, the addend the offset
  relative_32,         // REL32..REL32_5 with the bias below
  section_relative_32, // SECREL — the target's offset within its section
  section_index_16,    // SECTION — one-based index of the target's section
  unsupported,         // SECREL7, TOKEN, SREL32, PAIR, SSPAN32
};

struct relocation {
  /// Zero-based index of the section holding the site (`sections[…]`).
  std::uint16_t target_section = 0;
  /// Offset of the site within that section.
  std::uint32_t offset = 0;
  std::uint32_t symbol_index = 0;
  relocation_kind kind = relocation_kind::unsupported;
  /// REL32_N only: bytes between the site's end and the instruction's end;
  /// the applied displacement is target - (site + 4 + bias).
  std::uint8_t rel32_bias = 0;
};

/// One RUNTIME_FUNCTION from a `.pdata` section. Both drivers encode it as
/// three ADDR32NB relocations (begin, end, unwind) whose symbols name the
/// target sections — MSVC targets function-scope labels with one entry per
/// companion section, clang groups entries against plain section symbols —
/// so the association rides on the section, never on a name convention.
struct unwind_entry {
  /// Stored addends: the function bounds within its text section, and the
  /// unwind info within its `.xdata` section.
  std::uint32_t begin_offset = 0;
  std::uint32_t end_offset = 0;
  std::uint32_t unwind_offset = 0;
  std::uint16_t text_section = 0;  // zero-based; `section_class::text`
  std::uint16_t xdata_section = 0; // zero-based; named ".xdata"
  std::uint16_t pdata_section = 0; // zero-based; where this entry lives
};

struct object_file {
  std::vector<section> sections;
  std::vector<symbol> symbols;
  std::vector<relocation> relocations;
  std::vector<unwind_entry> unwind_table;
};

/// Parse an x86-64 COFF object. Throws std::runtime_error with a
/// human-readable reason on anything unexpected.
object_file parse_object(const std::uint8_t* data, std::size_t size);

} // namespace neko::pe
