#include "loader.hpp"

#include <elf.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "code_pages.hpp"
#include "object_file.hpp"
#include "process_symbols.hpp"
#include <neko/log.hpp>
#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/state_manager.hpp>
#include <neko/runtime/symbol_provider.hpp>

namespace neko::elf {
namespace {

constexpr std::uint64_t kSectionAlign = 16;

std::uint64_t align_up(std::uint64_t v, std::uint64_t a) {
  return (v + a - 1) / a * a;
}

/// x86-64 relocation types this loader understands.
// Keep the 32-bit type used by ELF64 relocation records.
// NOLINTNEXTLINE(performance-enum-size)
enum : std::uint32_t {
  kRelPc32 = R_X86_64_PC32,
  kRelPlt32 = R_X86_64_PLT32,
  kRel32 = R_X86_64_32,
  kRel32s = R_X86_64_32S,
  kRel64 = R_X86_64_64,
};

/// Only R_X86_64_PLT32 against an undefined symbol is (always) a call —
/// route those through an in-arena trampoline, since libc lives far outside
/// the ±2GiB reach of a `call rel32`. R_X86_64_PC32 against an undefined
/// symbol is a DATA reference (e.g. `stdout`): conflating the two once made
/// fflush(stdout) read trampoline bytes as a FILE* (SIGSEGV, caught by the
/// playground demo).
bool is_call_to_undefined(std::uint32_t type) {
  return type == kRelPlt32;
}

/// movabs rax, imm64 ; jmp rax — a 10-byte PLT for out-of-range externals.
void write_trampoline(std::uint8_t* out, std::uintptr_t target) {
  out[0] = 0x48;
  out[1] = 0xB8;
  std::memcpy(out + 2, &target, sizeof(target));
  out[10] = 0xFF;
  out[11] = 0xE0;
}

} // namespace

loader::loader(process_symbols& symbols, state_manager& state, code_substituter& substituter)
    : symbols_(symbols), state_(state), substituter_(substituter) {}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size) {
  const object_file obj = parse_object(object_data, size);

  // ---- 1. layout: text/rodata sections get arena offsets ---------------
  std::vector<std::uint64_t> section_offset(obj.sections.size(), 0);
  std::uint64_t image_size = 0;
  for (const auto& sec : obj.sections) {
    if (sec.cls != section_class::text && sec.cls != section_class::rodata) {
      continue;
    }
    image_size = align_up(image_size, kSectionAlign);
    section_offset[sec.index] = image_size;
    image_size += sec.size;
  }
  if (image_size == 0) {
    throw std::runtime_error("object file carries no code");
  }

  // ---- 2. undefined-symbol CALLS get in-arena trampolines --------------
  // Slot assignment happens before the arena is reserved: the trampolines
  // are part of the image, so their bytes must be covered by the
  // reservation. Growing image_size after reserve_code_near() would commit
  // a memcpy past the reserved page span.
  // Only the slot offsets are known here; resolve_external failures are
  // rejections, so resolve before any memory is reserved.
  std::unordered_map<std::uint32_t, std::uint64_t> trampoline_offset_for_symbol;
  for (const auto& rel : obj.relocations) {
    if (!is_call_to_undefined(rel.type) || rel.symbol_index == 0 ||
        rel.symbol_index >= obj.symbols.size()) {
      continue;
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    if (sym.section_index != SHN_UNDEF || trampoline_offset_for_symbol.count(rel.symbol_index)) {
      continue;
    }
    void* target = symbols_.resolve_external(sym.name);
    if (target == nullptr) {
      throw std::runtime_error("cannot resolve external symbol '" + sym.name +
                               "' — the process defines no such symbol and the dynamic linker "
                               "cannot see one either");
    }
    image_size = align_up(image_size, kSectionAlign);
    trampoline_offset_for_symbol[rel.symbol_index] = image_size;
    image_size += 12; // movabs rax, imm64 (10) + jmp rax (2)
  }

  // ---- 3. arena near the code being replaced ---------------------------
  // Hint = the old entry of the first function we will redirect.
  std::uintptr_t hint = 0;
  for (const auto& sym : obj.symbols) {
    if (sym.type != STT_FUNC || sym.section_index == 0 ||
        sym.section_index >= obj.sections.size()) {
      continue;
    }
    if (obj.sections[sym.section_index].cls != section_class::text) {
      continue;
    }
    if (auto old = symbols_.function_by_name(sym.name)) {
      hint = old->address;
      break;
    }
  }
  if (hint == 0) {
    throw std::runtime_error("no function in the object matches a live process symbol — nothing "
                             "to reload");
  }
  void* arena = substituter_.reserve_code_near(hint, image_size);
  if (arena == nullptr) {
    throw std::runtime_error("could not reserve executable memory near the target");
  }
  const auto base = reinterpret_cast<std::uintptr_t>(arena);

  // ---- 4. build the image ----------------------------------------------
  std::vector<std::uint8_t> image(image_size, 0);
  for (const auto& sec : obj.sections) {
    if (sec.cls != section_class::text && sec.cls != section_class::rodata) {
      continue;
    }
    std::memcpy(image.data() + section_offset[sec.index], sec.bytes.data(), sec.bytes.size());
  }
  for (const auto& [sym_index, offset] : trampoline_offset_for_symbol) {
    void* target = symbols_.resolve_external(obj.symbols[sym_index].name);
    write_trampoline(image.data() + offset, reinterpret_cast<std::uintptr_t>(target));
  }

  // ---- 5. data-section anchors for state preservation ------------------
  // The assembler folds static-variable accesses into
  // `<data section> + addend` relocations (st_value + RIP adjustment), so a
  // data section needs an OLD base address. We anchor it on the named
  // OBJECT symbols of that section: old_base = old_addr(sym) - sym.value.
  // All anchors must agree; drift means the layout changed.
  std::vector<std::optional<std::uintptr_t>> data_base(obj.sections.size());
  for (const auto& sym : obj.symbols) {
    if (sym.type != STT_OBJECT || sym.section_index >= obj.sections.size()) {
      continue;
    }
    if (obj.sections[sym.section_index].cls != section_class::data) {
      continue;
    }
    void* existing = state_.map_global(sym.name);
    if (existing == nullptr) {
      continue; // reported when the symbol itself is referenced
    }
    const auto anchor = reinterpret_cast<std::uintptr_t>(existing) - sym.value;
    auto& slot = data_base[sym.section_index];
    if (slot && *slot != anchor) {
      throw std::runtime_error("data section '" + obj.sections[sym.section_index].name +
                               "' has inconsistent state anchors — the global layout changed");
    }
    slot = anchor;
  }

  // ---- 6. symbol resolution ---------------------------------------------
  auto resolve = [&](const symbol& sym) -> std::uintptr_t {
    if (sym.section_index == SHN_UNDEF) {
      // Data/address references to undefined symbols resolve against
      // the main executable first: the static linker already
      // copy-relocated them (stdout et al.) into our own data segment,
      // which is the only thing a rel32 can reach. Truly external
      // targets fall through to dlsym — and may then legitimately fail
      // the range check below (a documented current boundary).
      if (symbols_.count_globals(sym.name) > 1) {
        throw std::runtime_error("ambiguous global '" + sym.name +
                                 "' — refusing to bind state by name");
      }
      if (auto existing = symbols_.global_by_name(sym.name)) {
        return existing->address;
      }
      if (auto fn = symbols_.function_by_name(sym.name)) {
        return fn->address;
      }
      void* external = symbols_.resolve_external(sym.name);
      if (external == nullptr) {
        throw std::runtime_error("cannot resolve external symbol '" + sym.name +
                                 "' — the process defines no such symbol and the dynamic linker "
                                 "cannot see one either");
      }
      return reinterpret_cast<std::uintptr_t>(external);
    }
    if (sym.section_index >= obj.sections.size()) {
      throw std::runtime_error("symbol '" + sym.name + "' has odd section index");
    }
    const auto& sec = obj.sections[sym.section_index];
    if (sec.cls == section_class::data) {
      if (sym.type == STT_SECTION) {
        // Section-relative access to static storage (see step 5).
        if (!data_base[sym.section_index]) {
          throw std::runtime_error("cannot map data section '" + sec.name +
                                   "' onto process state — no anchor symbol with "
                                   "existing storage");
        }
        return *data_base[sym.section_index];
      }
      // Mutable data: bind to existing storage so state survives.
      void* existing = state_.map_global(sym.name);
      if (existing == nullptr) {
        throw std::runtime_error("fresh code introduces mutable symbol '" + sym.name +
                                 "' with no existing storage — new globals are not supported yet");
      }
      if (auto old = symbols_.global_by_name(sym.name);
          old && sym.size != 0 && sym.size != old->size) {
        throw std::runtime_error(
            "global '" + sym.name + "' changed size (" + std::to_string(old->size) + " -> " +
            std::to_string(sym.size) +
            ") — changing the layout of existing globals is not supported yet");
      }
      return reinterpret_cast<std::uintptr_t>(existing);
    }
    if (sym.type == STT_SECTION) {
      return base + section_offset[sym.section_index]; // st_value is 0
    }
    return base + section_offset[sym.section_index] + sym.value;
  };

  // ---- 7. relocations of text sections ----------------------------------
  for (const auto& rel : obj.relocations) {
    if (rel.target_section >= obj.sections.size()) {
      continue;
    }
    const auto& target = obj.sections[rel.target_section];
    if (target.cls != section_class::text) {
      continue; // .rela.data/.rela.eh_frame etc. — not handled yet
    }
    if (rel.symbol_index >= obj.symbols.size()) {
      throw std::runtime_error("relocation with bad symbol index");
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    std::uintptr_t s = 0;
    if (is_call_to_undefined(rel.type) && sym.section_index == SHN_UNDEF) {
      const auto it = trampoline_offset_for_symbol.find(rel.symbol_index);
      if (it == trampoline_offset_for_symbol.end()) {
        throw std::runtime_error("internal: missing trampoline");
      }
      s = base + it->second;
    } else {
      s = resolve(sym);
    }
    const std::uint64_t write_at = section_offset[rel.target_section] + rel.offset;
    const std::uintptr_t p = base + write_at;
    std::int64_t value = 0;
    switch (rel.type) {
    case kRelPc32:
    case kRelPlt32:
      value = static_cast<std::int64_t>(s) + rel.addend - static_cast<std::int64_t>(p);
      if (value > 0x7FFF'FFFF || value < -0x8000'0000LL) {
        throw std::runtime_error("relocation out of rel32 range for '" + sym.name + "'");
      }
      std::memcpy(image.data() + write_at, &value, sizeof(std::int32_t));
      break;
    case kRel32:
    case kRel32s:
      value = static_cast<std::int64_t>(s) + rel.addend;
      if (value > 0x7FFF'FFFF || value < -0x8000'0000LL) {
        throw std::runtime_error("32-bit relocation does not fit for '" + sym.name + "'");
      }
      std::memcpy(image.data() + write_at, &value, sizeof(std::int32_t));
      break;
    case kRel64:
      value = static_cast<std::int64_t>(s) + rel.addend;
      std::memcpy(image.data() + write_at, &value, sizeof(std::int64_t));
      break;
    default:
      throw std::runtime_error("unsupported relocation type " + std::to_string(rel.type) +
                               " for '" + sym.name + "' (GOT-style relocs need -fno-pic code)");
    }
  }

  // ---- 8. what to redirect ----------------------------------------------
  loaded_image out;
  out.code = arena;
  out.code_size = image_size;

  // The fresh object's own symbol list, which is how the offline manifest is
  // asked "which translation unit is this?": two units that share one static
  // name rarely share all of them.
  std::vector<std::string> unit_symbols;
  unit_symbols.reserve(obj.symbols.size());
  for (const auto& sym : obj.symbols) {
    if (sym.type == STT_FUNC && sym.section_index != 0 && sym.section_index < obj.sections.size() &&
        obj.sections[sym.section_index].cls == section_class::text) {
      unit_symbols.push_back(sym.name);
    }
  }

  for (const auto& sym : obj.symbols) {
    if (sym.type != STT_FUNC || sym.section_index == 0 ||
        sym.section_index >= obj.sections.size()) {
      continue;
    }
    if (obj.sections[sym.section_index].cls != section_class::text) {
      continue;
    }
    std::optional<function_info> old = symbols_.function_by_name(sym.name);
    const auto duplicates = symbols_.count_functions(sym.name);

    if (duplicates > 1) {
      // Same-named static functions are ordinary in real code, and the symbol
      // table cannot say which is which. The binding can (a global name is
      // unique by construction), and the offline manifest can (it records
      // which source file defined each address).
      old = symbols_.function_in_unit(sym.name, sym.bind, unit_symbols);
      if (!old.has_value()) {
        throw std::runtime_error(
            "ambiguous function '" + sym.name + "' (" + std::to_string(duplicates) +
            " symbols share the name) — refusing to patch by name. `nekomata manifest "
            "<binary>` next to the executable would tell them apart");
      }
    } else if (sym.bind == STB_LOCAL && old.has_value() && !unit_symbols.empty()) {
      // A local name is local *to a translation unit*, so finding one by name
      // is not evidence that this is the same function: a fresh static helper
      // whose name another unit also uses would be redirected onto that other
      // unit's function. The manifest can tell them apart, and when it says
      // this is a different unit, the answer is still to refuse — the archive
      // has no business redirecting a function this object never replaced.
      //
      // Without a manifest there is nothing to check against, and the reload
      // proceeds as it always has: a manifest can only tighten this decision,
      // never loosen it.
      const auto same_unit = symbols_.function_in_unit(sym.name, sym.bind, unit_symbols);
      if (!same_unit.has_value()) {
        throw std::runtime_error(
            "ambiguous function '" + sym.name +
            "' — the symbol manifest does not "
            "attribute this file-static name to this object's translation unit; "
            "refusing to patch by name");
      }
      old = same_unit;
    }
    if (!old) {
      continue; // fresh code introduced a new static helper — fine, it
                // just lives in the arena; nothing to redirect
    }
    if (old->size != 0 && old->size < 8) {
      throw std::runtime_error("function '" + sym.name + "' is too small to patch safely (" +
                               std::to_string(old->size) + " bytes)");
    }
    function_replacement repl;
    repl.name = sym.name;
    repl.old_entry = old->address;
    repl.offset_in_image =
        static_cast<std::uint32_t>(section_offset[sym.section_index] + sym.value);
    out.replacements.push_back(std::move(repl));
  }

  // ---- 9. make it executable ---------------------------------------------
  if (!substituter_.commit_code(arena, image.data(), image.size())) {
    throw std::runtime_error("failed to commit code image");
  }

  for (const auto& repl : out.replacements) {
    neko::log(log_level::info, "  %s -> %p (+0x%x)\n", repl.name.c_str(),
              static_cast<void*>(reinterpret_cast<std::uint8_t*>(out.code) + repl.offset_in_image),
              repl.offset_in_image);
  }
  return out;
}

} // namespace neko::elf
