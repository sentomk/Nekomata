#include "image_layout.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace neko::pe {
namespace {

// Sections align to at least 16 bytes — x64 code and SIMD-friendly constant
// loads — and may never demand more than a page: the arena commits whole
// pages, and an over-aligned section would pin geometry the reservation
// cannot honor.
constexpr std::uint64_t kSectionAlign = 16;
constexpr std::uint64_t kMaxSectionAlign = 4096;
constexpr std::uint64_t kUnwindAlign = 4;
constexpr std::uint64_t kTrampolineSize = 13; // movabs r11, imm64 + jmp r11

std::uint64_t align_up(std::uint64_t value, std::uint64_t align) {
  return (value + align - 1) / align * align;
}

bool unwind_section(const section& sec) {
  return sec.name == ".pdata" || sec.name == ".xdata";
}

} // namespace

image_layout plan_image(const object_file& obj) {
  image_layout plan;

  for (const auto& sec : obj.sections) {
    if (sec.cls != section_class::text && sec.cls != section_class::rodata) {
      continue;
    }
    if (sec.align > kMaxSectionAlign) {
      throw std::runtime_error("section '" + sec.name + "' requires alignment " +
                               std::to_string(sec.align) +
                               " — over-aligned sections beyond a page are not supported yet");
    }
    plan.image_size = align_up(plan.image_size, std::max(kSectionAlign, sec.align));
    plan.code.push_back({sec.index, plan.image_size, sec.size});
    plan.image_size += sec.size;
  }
  if (plan.code.empty()) {
    throw std::runtime_error("object file carries no code");
  }

  // The unwind tail shares the image so its image-relative relocations
  // resolve against the same pseudo base the code uses.
  for (const auto& sec : obj.sections) {
    if (!unwind_section(sec)) {
      continue;
    }
    plan.image_size = align_up(plan.image_size, std::max(kUnwindAlign, sec.align));
    plan.unwind.push_back({sec.index, plan.image_size, sec.size});
    plan.image_size += sec.size;
  }

  // Functions defined in placed code sections, in symbol-table order.
  for (std::uint32_t index = 0; index < obj.symbols.size(); ++index) {
    const auto& sym = obj.symbols[index];
    if (sym.auxiliary || !sym.is_function() || sym.section_number < 1) {
      continue;
    }
    const auto& sec = obj.sections[static_cast<std::size_t>(sym.section_number) - 1];
    if (sec.cls != section_class::text) {
      continue;
    }
    const auto placement =
        std::find_if(plan.code.begin(), plan.code.end(),
                     [&](const section_placement& p) { return p.section == sec.index; });
    if (placement == plan.code.end()) {
      continue; // defensive: every code-class section is placed above
    }
    replacement_candidate candidate;
    candidate.symbol_index = index;
    candidate.name = sym.name;
    candidate.offset_in_image = static_cast<std::uint32_t>(placement->offset + sym.value);
    plan.functions.push_back(std::move(candidate));
  }

  // One trampoline slot per undefined external referenced through a
  // relative_32 site in a placed section. Common symbols are definitions
  // awaiting storage, not imports, so they never reserve a slot.
  std::vector<std::uint32_t> slotted;
  const auto is_placed = [&](std::uint16_t section_index) {
    return std::any_of(plan.code.begin(), plan.code.end(),
                       [&](const section_placement& p) { return p.section == section_index; });
  };
  for (const auto& rel : obj.relocations) {
    if (rel.kind != relocation_kind::relative_32 || rel.symbol_index >= obj.symbols.size() ||
        !is_placed(rel.target_section)) {
      continue;
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    if (sym.auxiliary || sym.section_number != 0 || sym.storage_class != 2 || sym.is_common()) {
      continue;
    }
    if (std::find(slotted.begin(), slotted.end(), rel.symbol_index) != slotted.end()) {
      continue;
    }
    slotted.push_back(rel.symbol_index);
    plan.image_size = align_up(plan.image_size, kSectionAlign);
    trampoline_slot slot;
    slot.symbol_index = rel.symbol_index;
    slot.name = sym.name;
    slot.offset_in_image = static_cast<std::uint32_t>(plan.image_size);
    plan.image_size += kTrampolineSize;
    plan.trampolines.push_back(std::move(slot));
  }

  return plan;
}

} // namespace neko::pe
