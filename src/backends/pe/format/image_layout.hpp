// image_layout — the placement plan for one fresh COFF object's arena image.
//
// Pure computation over a parsed object: which sections enter the executable
// image, at what offsets and alignments, where every function lands, and
// which undefined externals need an in-image trampoline slot. No memory is
// reserved and no symbol is resolved here; the runtime mini-link consumes
// the plan in one pass so its reservation already covers every byte
// (trampolines included — growing the image after reserve would commit a
// copy past the reserved span).
//
// Geometry, mirroring the ELF arena: code sections (text and read-only
// data, in section-index order) lead the image, each aligned to at least 16
// bytes and never beyond a page. The unwind tail follows: .pdata and .xdata
// ride the same image so ADDR32NB arithmetic stays consistent against one
// pseudo image base when the runtime registers them. Mutable data sections
// never enter this image — they belong to the state path's writable
// storage.
//
// Trampoline policy: COFF marks calls and data references with the same
// REL32 type, so every undefined external referenced through a relative_32
// site reserves a 13-byte slot (movabs r11, imm64; jmp r11 — r11 is
// volatile and the thunk is jump-only, so variadic callees keep their
// argument registers). A reference that later turns out to target data
// rather than code is rejected at resolution time; a code thunk cannot
// serve a load.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "object_file.hpp"

namespace neko::pe {

/// Where one COFF section lands inside the executable image.
struct section_placement {
  std::uint16_t section = 0; // zero-based index into object_file::sections
  std::uint64_t offset = 0;  // image-relative
  std::uint64_t size = 0;
};

/// One function the mini-link will offer for entry redirection. The old
/// entry stays zero here; resolving it against the live process is the
/// runtime's business.
struct replacement_candidate {
  std::uint32_t symbol_index = 0;
  std::string name; // decorated bytes, as the drivers emit them
  std::uint32_t offset_in_image = 0;
};

/// An undefined external referenced through a relative_32 site: its
/// trampoline slot, awaiting a live definition or a generation sibling.
struct trampoline_slot {
  std::uint32_t symbol_index = 0;
  std::string name;
  std::uint32_t offset_in_image = 0;
};

struct image_layout {
  /// Code sections in offset order; text and read-only data interleaved by
  /// section index, exactly as the ELF arena lays them out.
  std::vector<section_placement> code;
  /// The unwind tail (.pdata, .xdata), after every code section.
  std::vector<section_placement> unwind;
  std::uint64_t image_size = 0;
  std::vector<replacement_candidate> functions;
  std::vector<trampoline_slot> trampolines;
};

/// Plan the arena image for one parsed object. Throws std::runtime_error
/// with a human-readable reason when the object's shape cannot be placed:
/// no code at all, or a section demanding more than a page of alignment.
image_layout plan_image(const object_file& obj);

} // namespace neko::pe
