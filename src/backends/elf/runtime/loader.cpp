#include "loader.hpp"

#include <elf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "format/object_file.hpp"
#include "runtime/code_pages.hpp"
#include "runtime/process_symbols.hpp"
#include <neko/backend/code_substituter.hpp>
#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/log.hpp>

namespace neko::elf {
namespace {

constexpr std::uint64_t kSectionAlign = 16;

/// Sections may not ask for more than a page of alignment: the arena base
/// is page-aligned, so anything larger is not representable, and absurd
/// values would overflow align_up() before any check could fire.
constexpr std::uint64_t kMaxSectionAlign = 4096;

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

/// movabs r11, imm64 ; jmp r11 — a 13-byte PLT for out-of-range externals.
/// Never rax: at a call site AL is live — it carries the SSE-argument
/// count into variadic callees (printf & co.). Clobbering rax left AL at
/// the mercy of the target address's low byte; when that byte was 0 the
/// callee skipped its SSE save area and every %.1f silently read zeros
/// (caught by the playground demo, whose HUD doubles all printed 0.0).
/// r11 is the classic PLT scratch register; note its jmp needs REX.B,
/// making this stub one byte longer than the rax form.
void write_trampoline(std::uint8_t* out, std::uintptr_t target) {
  out[0] = 0x49; // REX.WB
  out[1] = 0xBB; // movabs r11, imm64
  std::memcpy(out + 2, &target, sizeof(target));
  out[10] = 0x41; // REX.B
  out[11] = 0xFF;
  out[12] = 0xE3; // jmp r11
}

std::optional<generation_fixup_kind> data_fixup_kind(std::uint32_t relocation_type) {
  switch (relocation_type) {
  case kRelPc32:
    return generation_fixup_kind::relative_32;
  case kRel32:
    return generation_fixup_kind::absolute_32;
  case kRel32s:
    return generation_fixup_kind::absolute_32_signed;
  case kRel64:
    return generation_fixup_kind::absolute_64;
  default:
    return std::nullopt;
  }
}

std::uint64_t data_fixup_size(generation_fixup_kind kind) {
  switch (kind) {
  case generation_fixup_kind::relative_32:
  case generation_fixup_kind::absolute_32:
  case generation_fixup_kind::absolute_32_signed:
    return sizeof(std::int32_t);
  case generation_fixup_kind::absolute_64:
    return sizeof(std::int64_t);
  case generation_fixup_kind::function_trampoline:
    break;
  }
  return 0;
}

std::uint64_t write_data_fixup(std::uint8_t* out, generation_fixup_kind kind, std::uintptr_t target,
                               std::uintptr_t place, std::int64_t addend,
                               std::string_view symbol_name) {
  std::int64_t value = 0;
  switch (kind) {
  case generation_fixup_kind::relative_32:
    value = static_cast<std::int64_t>(target) + addend - static_cast<std::int64_t>(place);
    if (value > 0x7FFF'FFFF || value < -0x8000'0000LL) {
      throw std::runtime_error("relocation out of rel32 range for '" + std::string(symbol_name) +
                               "'");
    }
    std::memcpy(out, &value, sizeof(std::int32_t));
    return sizeof(std::int32_t);
  case generation_fixup_kind::absolute_32:
  case generation_fixup_kind::absolute_32_signed:
    value = static_cast<std::int64_t>(target) + addend;
    if (value > 0x7FFF'FFFF || value < -0x8000'0000LL) {
      throw std::runtime_error("32-bit relocation does not fit for '" + std::string(symbol_name) +
                               "'");
    }
    std::memcpy(out, &value, sizeof(std::int32_t));
    return sizeof(std::int32_t);
  case generation_fixup_kind::absolute_64:
    value = static_cast<std::int64_t>(target) + addend;
    std::memcpy(out, &value, sizeof(std::int64_t));
    return sizeof(std::int64_t);
  case generation_fixup_kind::function_trampoline:
    break;
  }
  throw std::runtime_error("unsupported data fixup for '" + std::string(symbol_name) + "'");
}

bool has_external_binding(std::uint8_t binding) {
  if (binding == STB_GLOBAL || binding == STB_WEAK) {
    return true;
  }
#ifdef STB_GNU_UNIQUE
  return binding == STB_GNU_UNIQUE;
#else
  return false;
#endif
}

std::optional<std::string> state_identity(const symbol& sym, std::string_view source_path) {
  if (has_external_binding(sym.bind)) {
    return "external:" + sym.name;
  }
  if (sym.bind == STB_LOCAL && !source_path.empty()) {
    return "local:" + std::to_string(source_path.size()) + ":" + std::string(source_path) +
           sym.name;
  }
  return std::nullopt;
}

} // namespace

loader::loader(process_symbols& symbols, state_manager& state, code_substituter& substituter)
    : symbols_(symbols), state_(state), substituter_(substituter) {}

const loader::committed_state* loader::find_committed_state(std::string_view identity) const {
  const auto found =
      std::find_if(committed_state_.begin(), committed_state_.end(),
                   [identity](const committed_state& state) { return state.identity == identity; });
  return found == committed_state_.end() ? nullptr : &*found;
}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size) {
  return load(object_data, size, {});
}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size,
                          std::string_view source_path) {
  const object_file obj = parse_object(object_data, size);

  for (const auto& sym : obj.symbols) {
    if (sym.type == STT_TLS) {
      throw std::runtime_error("thread-local symbol '" + sym.name +
                               "' is not supported by hot reload yet");
    }
    if (sym.name.starts_with("_ZGV") || sym.name == "__cxa_guard_acquire" ||
        sym.name == "__cxa_guard_release" || sym.name == "__cxa_guard_abort" ||
        sym.name == "__cxa_atexit") {
      throw std::runtime_error(
          "dynamic initialization or destruction of mutable state is not supported yet");
    }
  }
  for (const auto& rel : obj.relocations) {
    if (rel.target_section >= obj.sections.size()) {
      continue;
    }
    const auto& target = obj.sections[rel.target_section];
    if (target.cls == section_class::data) {
      throw std::runtime_error(
          "section '" + target.name +
          "' carries relocations — mutable initializers requiring relocations are not "
          "supported yet");
    }
  }

  // ---- 1. layout: text/rodata sections get arena offsets ---------------
  std::vector<std::uint64_t> section_offset(obj.sections.size(), 0);
  std::uint64_t image_size = 0;
  for (const auto& sec : obj.sections) {
    if (sec.cls != section_class::text && sec.cls != section_class::rodata) {
      continue;
    }
    if (sec.align > kMaxSectionAlign) {
      throw std::runtime_error("section '" + sec.name + "' requires alignment " +
                               std::to_string(sec.align) +
                               " — over-aligned sections beyond a page are not supported yet");
    }
    image_size = align_up(image_size, std::max(kSectionAlign, sec.align));
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
  std::unordered_map<std::uint32_t, bool> symbol_pending_in_generation;
  std::vector<generation_fixup> pending_fixups;
  for (const auto& rel : obj.relocations) {
    if (!is_call_to_undefined(rel.type) || rel.symbol_index == 0 ||
        rel.symbol_index >= obj.symbols.size()) {
      continue;
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    if (sym.section_index != SHN_UNDEF || trampoline_offset_for_symbol.count(rel.symbol_index)) {
      continue;
    }
    void* target = symbols_.resolve_external(sym.name, STT_FUNC, sym.bind);
    if (target == nullptr) {
      // No live definition — but a sibling object of the same generation may
      // define this symbol freshly. Reserve the slot, leave a placeholder
      // trampoline, and let link_generation() resolve or reject it once the
      // whole candidate set is loaded.
      symbol_pending_in_generation[rel.symbol_index] = true;
    }
    image_size = align_up(image_size, kSectionAlign);
    trampoline_offset_for_symbol[rel.symbol_index] = image_size;
    image_size += 13; // movabs r11, imm64 (10) + jmp r11 (3, needs REX.B)
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
  auto allocation = substituter_.reserve_code_near(hint, image_size);
  if (allocation == nullptr) {
    throw std::runtime_error("could not reserve executable memory near the target");
  }
  void* arena = allocation->data();
  const auto base = reinterpret_cast<std::uintptr_t>(arena);

  // ---- 4. build the image ----------------------------------------------
  std::vector<std::uint8_t> image(image_size, 0);
  for (const auto& sec : obj.sections) {
    if (sec.cls != section_class::text && sec.cls != section_class::rodata) {
      continue;
    }
    if (sec.bytes.empty()) {
      continue; // an empty section carries no bytes; its data pointer may be null
    }
    std::memcpy(image.data() + section_offset[sec.index], sec.bytes.data(), sec.bytes.size());
  }
  for (const auto& [sym_index, offset] : trampoline_offset_for_symbol) {
    const auto& sym = obj.symbols[sym_index];
    if (symbol_pending_in_generation.count(sym_index) != 0) {
      write_trampoline(image.data() + offset, 0); // patched by link_generation()
      pending_fixups.push_back({sym.name, generation_symbol_kind::function,
                                generation_fixup_kind::function_trampoline,
                                static_cast<std::uint32_t>(offset)});
      continue;
    }
    void* target = symbols_.resolve_external(sym.name, STT_FUNC, sym.bind);
    write_trampoline(image.data() + offset, reinterpret_cast<std::uintptr_t>(target));
  }

  loaded_image out;
  out.allocation = std::move(allocation);
  out.pending_fixups = std::move(pending_fixups);

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

  struct mapped_state {
    std::uintptr_t address;
    std::uint64_t size;
  };
  std::vector<std::optional<mapped_state>> state_by_symbol(obj.symbols.size());
  for (std::size_t symbol_index = 0; symbol_index < obj.symbols.size(); ++symbol_index) {
    const auto& sym = obj.symbols[symbol_index];
    if (sym.type != STT_OBJECT || sym.section_index >= obj.sections.size()) {
      continue;
    }
    const auto& sec = obj.sections[sym.section_index];
    if (sec.cls != section_class::data) {
      continue;
    }
    if (sym.size == 0 || sym.value > sec.size || sym.size > sec.size - sym.value) {
      throw std::runtime_error("mutable symbol '" + sym.name +
                               "' has an invalid or unknown storage extent");
    }

    const auto identity = state_identity(sym, source_path);
    if (identity) {
      if (const auto* committed = find_committed_state(*identity)) {
        if (committed->size != sym.size || committed->alignment != sec.align) {
          throw std::runtime_error(
              "global '" + sym.name + "' changed layout (size " + std::to_string(committed->size) +
              " -> " + std::to_string(sym.size) + ", alignment " +
              std::to_string(committed->alignment) + " -> " + std::to_string(sec.align) +
              ") — changing the layout of existing globals is not "
              "supported yet");
        }
        state_by_symbol[symbol_index] = mapped_state{committed->address, committed->size};
        continue;
      }
    }

    if (void* existing = state_.map_global(sym.name)) {
      if (auto old = symbols_.global_by_name(sym.name); old && sym.size != old->size) {
        throw std::runtime_error(
            "global '" + sym.name + "' changed size (" + std::to_string(old->size) + " -> " +
            std::to_string(sym.size) +
            ") — changing the layout of existing globals is not supported yet");
      }
      state_by_symbol[symbol_index] =
          mapped_state{reinterpret_cast<std::uintptr_t>(existing), sym.size};
      continue;
    }

    if (!identity) {
      throw std::runtime_error("fresh file-local mutable symbol '" + sym.name +
                               "' has no source identity — use a managed reload group or "
                               "watch(object, source)");
    }
    if (sec.align > kMaxSectionAlign) {
      throw std::runtime_error("mutable symbol '" + sym.name + "' requires alignment " +
                               std::to_string(sec.align) +
                               " — over-aligned globals beyond a page are not supported yet");
    }
    auto state_allocation = substituter_.reserve_writable_near(hint, sym.size);
    if (state_allocation == nullptr) {
      throw std::runtime_error("could not reserve writable memory near the target for global '" +
                               sym.name + "'");
    }
    if (sec.bytes.empty()) {
      std::memset(state_allocation->data(), 0, static_cast<std::size_t>(sym.size));
    } else {
      std::memcpy(state_allocation->data(), sec.bytes.data() + sym.value,
                  static_cast<std::size_t>(sym.size));
    }
    const auto address = reinterpret_cast<std::uintptr_t>(state_allocation->data());
    state_by_symbol[symbol_index] = mapped_state{address, sym.size};
    out.state_definitions.push_back({*identity, sym.name, address, sym.size, sec.align, true});
    out.state_allocations.push_back(std::move(state_allocation));
  }

  for (std::size_t symbol_index = 0; symbol_index < obj.symbols.size(); ++symbol_index) {
    const auto& sym = obj.symbols[symbol_index];
    if (sym.type != STT_OBJECT || !has_external_binding(sym.bind) ||
        !state_by_symbol[symbol_index]) {
      continue;
    }
    out.exported_symbols.push_back(
        {sym.name, generation_symbol_kind::object, 0, state_by_symbol[symbol_index]->address});
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
      void* external = symbols_.resolve_external(sym.name, sym.type, sym.bind);
      if (external == nullptr) {
        throw std::runtime_error("cannot resolve external symbol '" + sym.name +
                                 "' — the process has no link-visible definition and the dynamic "
                                 "linker cannot see one either");
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
      const auto index = static_cast<std::size_t>(&sym - obj.symbols.data());
      if (index >= state_by_symbol.size() || !state_by_symbol[index]) {
        throw std::runtime_error("cannot map mutable symbol '" + sym.name + "' onto live state");
      }
      return state_by_symbol[index]->address;
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
      // .rela.eh_frame is skipped on purpose: the arena copy is never
      // registered for unwinding, so it stays inert. Every OTHER read-only
      // section with relocations is a trap: its bytes are copied into the
      // arena and USED by the fresh code, but the pointer slots would stay
      // zero — a vtable, jump table or const pointer array built that way
      // crashes at first use, not at load time. Refuse loudly instead.
      if (target.cls == section_class::rodata && target.name.rfind(".eh_frame", 0) != 0) {
        throw std::runtime_error("section '" + target.name +
                                 "' carries relocations — relocated constant tables (vtables, jump "
                                 "tables) are not supported yet");
      }
      continue;
    }
    if (rel.symbol_index >= obj.symbols.size()) {
      throw std::runtime_error("relocation with bad symbol index");
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    const std::uint64_t write_at = section_offset[rel.target_section] + rel.offset;
    const std::uintptr_t p = base + write_at;
    std::uintptr_t s = 0;
    if (is_call_to_undefined(rel.type) && sym.section_index == SHN_UNDEF) {
      const auto it = trampoline_offset_for_symbol.find(rel.symbol_index);
      if (it == trampoline_offset_for_symbol.end()) {
        throw std::runtime_error("internal: missing trampoline");
      }
      s = base + it->second;
    } else if (sym.type == STT_SECTION && sym.section_index < obj.sections.size() &&
               obj.sections[sym.section_index].cls == section_class::data) {
      std::int64_t target_offset = rel.addend;
      if (rel.type == kRelPc32 || rel.type == kRelPlt32) {
        if (target_offset > std::numeric_limits<std::int64_t>::max() - 4) {
          throw std::runtime_error("data-section relocation addend overflows");
        }
        target_offset += 4;
      }
      std::optional<std::uintptr_t> mapped_base;
      if (target_offset >= 0) {
        const auto offset = static_cast<std::uint64_t>(target_offset);
        for (std::size_t index = 0; index < obj.symbols.size(); ++index) {
          const auto& candidate = obj.symbols[index];
          if (candidate.type != STT_OBJECT || candidate.section_index != sym.section_index ||
              !state_by_symbol[index] || offset < candidate.value ||
              offset - candidate.value >= candidate.size) {
            continue;
          }
          mapped_base = state_by_symbol[index]->address - candidate.value;
          break;
        }
      }
      if (mapped_base) {
        s = *mapped_base;
      } else {
        s = resolve(sym);
      }
    } else if (sym.section_index == SHN_UNDEF) {
      if (void* external = symbols_.resolve_external(sym.name, sym.type, sym.bind)) {
        s = reinterpret_cast<std::uintptr_t>(external);
      } else {
        const auto kind = data_fixup_kind(rel.type);
        if (!kind) {
          throw std::runtime_error("cannot resolve external symbol '" + sym.name +
                                   "' — the process has no link-visible definition and the "
                                   "dynamic linker cannot see one either");
        }
        const auto target_kind = sym.type == STT_FUNC ? generation_symbol_kind::function
                                                      : generation_symbol_kind::object;
        out.pending_fixups.push_back(
            {sym.name, target_kind, *kind, static_cast<std::uint32_t>(write_at), rel.addend});
        const auto width = data_fixup_size(*kind);
        if (write_at > image.size() || width > image.size() - write_at) {
          throw std::runtime_error("relocation for '" + sym.name + "' is outside the code image");
        }
        std::memset(image.data() + write_at, 0, static_cast<std::size_t>(width));
        continue;
      }
    } else {
      s = resolve(sym);
    }
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
  // Functions that end up with no redirect are not necessarily wrong (a new
  // static helper has nothing to replace), but a skipped name that LIVE code
  // also carries is how signature changes and overloads quietly keep running
  // old code. Record every skip and report them after the load.
  std::vector<std::pair<std::string, std::size_t>> not_redirected;

  for (const auto& sym : obj.symbols) {
    if (sym.type != STT_FUNC || sym.section_index == 0 ||
        sym.section_index >= obj.sections.size()) {
      continue;
    }
    if (obj.sections[sym.section_index].cls != section_class::text) {
      continue;
    }
    if (sym.bind == STB_GLOBAL || sym.bind == STB_WEAK || sym.bind == STB_GNU_UNIQUE) {
      // Sibling objects of the same generation may call this fresh body.
      out.exported_symbols.push_back(
          {sym.name, generation_symbol_kind::function,
           static_cast<std::uint32_t>(section_offset[sym.section_index] + sym.value)});
    }
    std::optional<function_info> old = symbols_.function_by_name(sym.name);
    const auto duplicates = symbols_.count_functions(sym.name);

    if (duplicates > 1) {
      // Same-named static functions are ordinary in real code. The build
      // integration supplies the source identity and the offline manifest
      // maps that exact source/name pair to one process address.
      old = symbols_.function_in_source(sym.name, sym.bind, source_path);
      if (!old.has_value()) {
        // A global with no old global candidate, or a local absent from a
        // manifest that otherwise knows this source, is a newly introduced
        // function. It stays in the fresh arena and redirects nothing.
        if (sym.bind == STB_GLOBAL || symbols_.contains_source(source_path)) {
          not_redirected.emplace_back(sym.name, duplicates);
          continue;
        }
        throw std::runtime_error(
            "ambiguous function '" + sym.name + "' (" + std::to_string(duplicates) +
            " symbols share the name) — refusing to patch by name; use "
            "watch(object, source) with a symbol manifest to identify its translation unit");
      }
    } else if (sym.bind == STB_LOCAL && old.has_value()) {
      // A local name is local *to a translation unit*, so finding one by name
      // is not evidence that this is the same function: a fresh static helper
      // whose name another unit also uses would be redirected onto that other
      // unit's function. Exact source identity can confirm the redirect; its
      // absence or a manifest mismatch must refuse rather than guess.
      const auto same_unit = symbols_.function_in_source(sym.name, sym.bind, source_path);
      if (!same_unit.has_value()) {
        if (symbols_.contains_source(source_path)) {
          not_redirected.emplace_back(sym.name, duplicates);
          continue; // a new file-static function in a known translation unit
        }
        throw std::runtime_error(
            "ambiguous function '" + sym.name +
            "' — no symbol manifest entry matches "
            "this file-static name and source identity; refusing to patch by name");
      }
      old = same_unit;
    }
    if (!old) {
      // Fresh code introduced a new static helper — fine, it just lives in
      // the arena; nothing to redirect. When the process happens to carry
      // the same name, say so: that is the signature-change smell.
      not_redirected.emplace_back(sym.name, duplicates);
      continue;
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
  if (!substituter_.commit_code(*out.allocation, image.data(), image.size())) {
    throw std::runtime_error("failed to commit code image");
  }

  for (const auto& repl : out.replacements) {
    neko::log(
        log_level::info, "  %s -> %p (+0x%x)\n", repl.name.c_str(),
        static_cast<void*>(reinterpret_cast<std::uint8_t*>(out.code()) + repl.offset_in_image),
        repl.offset_in_image);
  }
  for (const auto& [name, live_count] : not_redirected) {
    if (live_count > 1) {
      neko::log(log_level::warn,
                "not redirected: '%s' — %zu live functions share the name and none was "
                "confirmed as this unit's; keeping old code\n",
                name.c_str(), live_count);
    } else {
      neko::log(log_level::info,
                "not redirected: '%s' — no live entry to replace; the fresh body stays "
                "in the arena\n",
                name.c_str());
    }
  }
  return out;
}

void loader::link_generation(std::span<loaded_image* const> images) {
  struct linked_symbol {
    generation_symbol_kind kind;
    std::uintptr_t address;
  };

  // Prefer the candidate set: a sibling's fresh body is the new definition.
  std::unordered_map<std::string_view, linked_symbol> exported;
  for (const auto* image : images) {
    const auto base = reinterpret_cast<std::uintptr_t>(image->code());
    for (const auto& symbol : image->exported_symbols) {
      const auto address = symbol.kind == generation_symbol_kind::function
                               ? base + symbol.offset_in_image
                               : symbol.address;
      if (address == 0) {
        throw std::runtime_error("generation symbol '" + symbol.name + "' has no address");
      }
      const auto [existing, inserted] =
          exported.emplace(symbol.name, linked_symbol{symbol.kind, address});
      if (!inserted && existing->second.kind != symbol.kind) {
        throw std::runtime_error("generation symbol '" + symbol.name +
                                 "' has conflicting definition kinds");
      }
      if (!inserted && symbol.kind == generation_symbol_kind::object) {
        throw std::runtime_error("generation defines mutable symbol '" + symbol.name +
                                 "' more than once");
      }
    }
  }
  for (auto* image : images) {
    for (const auto& fixup : image->pending_fixups) {
      const auto found = exported.find(fixup.symbol_name);
      if (found == exported.end()) {
        throw std::runtime_error("cannot resolve external symbol '" + fixup.symbol_name +
                                 "' — the process has no link-visible definition and the dynamic "
                                 "linker cannot see one either");
      }
      if (found->second.kind != fixup.target_kind) {
        throw std::runtime_error("generation symbol '" + fixup.symbol_name +
                                 "' has an incompatible target kind");
      }
      if (fixup.kind == generation_fixup_kind::function_trampoline) {
        if (fixup.target_kind != generation_symbol_kind::function) {
          throw std::runtime_error("unsupported generation fixup for '" + fixup.symbol_name + "'");
        }
        std::uint8_t trampoline[13];
        write_trampoline(trampoline, found->second.address);
        if (!substituter_.rewrite_reservation(*image->allocation, fixup.offset_in_image, trampoline,
                                              sizeof(trampoline))) {
          throw std::runtime_error("cannot patch cross-object call to '" + fixup.symbol_name +
                                   "' inside its code image");
        }
        continue;
      }

      std::uint8_t bytes[sizeof(std::int64_t)]{};
      const auto place = reinterpret_cast<std::uintptr_t>(image->code()) + fixup.offset_in_image;
      const auto width = write_data_fixup(bytes, fixup.kind, found->second.address, place,
                                          fixup.addend, fixup.symbol_name);
      if (!substituter_.rewrite_reservation(*image->allocation, fixup.offset_in_image, bytes,
                                            width)) {
        throw std::runtime_error("cannot patch cross-object reference to '" + fixup.symbol_name +
                                 "' inside its code image");
      }
    }
    image->pending_fixups.clear();
  }
}

void loader::prepare_generation_commit(std::span<loaded_image* const> images) {
  std::vector<const backend::state_definition*> introduced;
  for (const auto* image : images) {
    for (const auto& definition : image->state_definitions) {
      if (!definition.introduced) {
        continue;
      }
      if (const auto* committed = find_committed_state(definition.identity)) {
        throw std::runtime_error("global '" + definition.name +
                                 "' was concurrently introduced with different storage at " +
                                 std::to_string(committed->address));
      }
      const auto duplicate = std::find_if(introduced.begin(), introduced.end(),
                                          [&](const backend::state_definition* candidate) {
                                            return candidate->identity == definition.identity;
                                          });
      if (duplicate != introduced.end()) {
        throw std::runtime_error("generation defines mutable symbol '" + definition.name +
                                 "' more than once");
      }
      introduced.push_back(&definition);
    }
  }
  committed_state_.reserve(committed_state_.size() + introduced.size());
}

void loader::commit_generation(std::span<loaded_image* const> images) noexcept {
  for (auto* image : images) {
    for (auto& definition : image->state_definitions) {
      if (!definition.introduced) {
        continue;
      }
      committed_state_.push_back({std::move(definition.identity), std::move(definition.name),
                                  definition.address, definition.size, definition.alignment});
      definition.introduced = false;
    }
  }
}

} // namespace neko::elf
