// object_file — minimal COFF x86-64 object (.obj) reader for the PE backend.
//
// Just enough for hot reload: section classification, alignment, and the raw
// bytes of the sections the loader places in its arena. Deliberately not a
// general COFF parser — it rejects everything that is not an x86-64 object
// from a supported driver family (MSVC and clang-cl), and ignores what it
// does not need. The symbol table and relocations arrive with their own
// commits; debug streams and unwind tables are classified as `other`.
//
// Surveyed shapes (2026-09-18, both drivers, /Od with and without /Gy): the
// section index is the identity — duplicate names such as `.text` or
// `.text$mn` are normal; COMDAT is unavoidable (inline functions, string
// literals, /Gy); classification follows the IMAGE_SCN_* flags just as the
// ELF reader classifies by sh_flags, with name-based exceptions for tables
// that must never enter the arena.

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
  /// objects. Selection and association live in the section symbol's
  /// auxiliary record and arrive with symbol parsing.
  bool comdat = false;
};

struct object_file {
  std::vector<section> sections;
};

/// Parse an x86-64 COFF object. Throws std::runtime_error with a
/// human-readable reason on anything unexpected.
object_file parse_object(const std::uint8_t* data, std::size_t size);

} // namespace neko::pe
