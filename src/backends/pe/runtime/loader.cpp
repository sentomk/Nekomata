#include "runtime/loader.hpp"

#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/log.hpp>

#include "format/image_layout.hpp"
#include "format/object_file.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neko::pe {
namespace {

using backend::function_replacement;
using backend::generation_fixup;
using backend::generation_fixup_kind;
using backend::generation_symbol;
using backend::generation_symbol_kind;
using backend::loaded_image;

/// movabs r11, imm64 ; jmp r11 — a 13-byte PLT for externals. Never rax: at a
/// call site AL carries the SSE-argument count into variadic callees, and
/// clobbering it corrupted every floating-point argument the playground
/// demo printed (the ELF backend's hard-won lesson; same ABI here).
void write_trampoline(std::uint8_t* out, std::uintptr_t target) {
  out[0] = 0x49; // REX.WB
  out[1] = 0xBB; // movabs r11, imm64
  std::memcpy(out + 2, &target, sizeof(target));
  out[10] = 0x41; // REX.B
  out[11] = 0xFF;
  out[12] = 0xE3; // jmp r11
}

std::int32_t read_stored32(const std::uint8_t* at) {
  std::int32_t value = 0;
  std::memcpy(&value, at, sizeof(value));
  return value;
}

std::int64_t read_stored64(const std::uint8_t* at) {
  std::int64_t value = 0;
  std::memcpy(&value, at, sizeof(value));
  return value;
}

bool unwind_section(const section& sec) {
  return sec.name == ".pdata" || sec.name == ".xdata";
}

constexpr std::uint64_t kUnplaced = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxSectionAlign = 4096;

/// One live-process resolution of an undefined external. Functions and data
/// live in different DIA streams, so both are probed; the hitting stream
/// decides the symbol's shape. Ambiguity (count > 1) refuses rather than
/// guesses.
struct external_address {
  std::uintptr_t address = 0;
  bool found = false;
  bool is_function = false;
};

external_address resolve_external(backend::symbol_provider& symbols, const symbol& sym) {
  external_address out;
  if (symbols.count_functions(sym.name) == 1) {
    if (auto hit = symbols.function_by_name(sym.name)) {
      out.address = hit->address;
      out.found = true;
      out.is_function = true;
      return out;
    }
  }
  if (symbols.count_globals(sym.name) == 1) {
    if (auto hit = symbols.global_by_name(sym.name)) {
      out.address = hit->address;
      out.found = true;
    }
  }
  return out;
}

/// Link-level identity of one mutable symbol: externally bound names are
/// process-wide, file-local names include their source identity. Without a
/// source path a file-local identity does not exist and fresh storage for
/// it refuses.
std::optional<std::string> state_identity(const symbol& sym, std::string_view source_path) {
  if (sym.storage_class == 2) {
    return "external:" + sym.name;
  }
  if (sym.storage_class == 3 && !source_path.empty()) {
    return "local:" + std::to_string(source_path.size()) + ":" + std::string(source_path) +
           sym.name;
  }
  return std::nullopt;
}

/// The drivers' function-local static guard family and atexit registration:
/// any occurrence means dynamic initialization or destruction of mutable
/// state, which no reload path supports.
bool dynamic_state_symbol(std::string_view name) {
  return name == "atexit" || name == "_onexit" || name == "_Init_thread_header" ||
         name == "_Init_thread_footer" || name == "_Init_thread_abort";
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

} // namespace

loader::loader(backend::symbol_provider& symbols, backend::state_manager& state,
               backend::code_substituter& substituter)
    : symbols_(symbols), state_(state), substituter_(substituter) {}

const loader::committed_state* loader::find_committed_state(std::string_view identity) const {
  const auto found = std::find_if(
      committed_state_.begin(), committed_state_.end(),
      [&](const committed_state& candidate) { return candidate.identity == identity; });
  return found == committed_state_.end() ? nullptr : &*found;
}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size) {
  return load(object_data, size, {});
}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size,
                          std::string_view source_path) {
  const object_file obj = parse_object(object_data, size);

  for (const auto& sec : obj.sections) {
    if (sec.name.rfind(".tls", 0) == 0) {
      throw std::runtime_error("section '" + sec.name +
                               "' is thread-local — thread-local storage is not supported by "
                               "hot reload yet");
    }
    if (sec.name.rfind(".CRT$", 0) == 0) {
      throw std::runtime_error("section '" + sec.name +
                               "' registers dynamic initialization or destruction — not "
                               "supported yet");
    }
  }
  for (const auto& sym : obj.symbols) {
    if (!sym.auxiliary && dynamic_state_symbol(sym.name)) {
      throw std::runtime_error(
          "dynamic initialization or destruction of mutable state is not supported yet ('" +
          sym.name + "')");
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

  const image_layout plan = plan_image(obj);

  // Reserve near the old entry of the first function we will redirect.
  std::uintptr_t hint = 0;
  for (const auto& candidate : plan.functions) {
    if (symbols_.count_functions(candidate.name) == 1) {
      if (auto old = symbols_.function_by_name(candidate.name)) {
        hint = old->address;
        break;
      }
    }
  }
  if (hint == 0) {
    throw std::runtime_error("no function in the object matches a live process symbol — nothing "
                             "to reload");
  }
  auto allocation = substituter_.reserve_code_near(hint, plan.image_size);
  if (allocation == nullptr) {
    throw std::runtime_error("could not reserve executable memory near the target");
  }
  const auto base = reinterpret_cast<std::uintptr_t>(allocation->data());

  // Copy every placed section — code and the unwind tail — into the image.
  std::vector<std::uint8_t> image(static_cast<std::size_t>(plan.image_size), 0);
  std::vector<std::uint64_t> placement_of(obj.sections.size(), kUnplaced);
  const auto copy = [&](const section_placement& p) {
    placement_of[p.section] = p.offset;
    const auto& sec = obj.sections[p.section];
    if (!sec.bytes.empty()) {
      std::memcpy(image.data() + p.offset, sec.bytes.data(),
                  static_cast<std::size_t>(sec.bytes.size()));
    }
  };
  std::for_each(plan.code.begin(), plan.code.end(), copy);
  std::for_each(plan.unwind.begin(), plan.unwind.end(), copy);

  // Trampolines: resolved externals jump straight to the live body;
  // unresolved ones hold a placeholder patched by link_generation(). An
  // external resolved as data keeps a placeholder too — its reference sites
  // take the direct address below, because a load cannot cross a jump.
  std::unordered_map<std::uint32_t, std::uint32_t> slot_for_symbol;
  std::vector<std::uint32_t> data_resolved_slots;
  loaded_image out;
  out.allocation = std::move(allocation);
  for (const auto& slot : plan.trampolines) {
    slot_for_symbol[slot.symbol_index] = slot.offset_in_image;
    const auto& sym = obj.symbols[slot.symbol_index];
    const auto external = resolve_external(symbols_, sym);
    if (external.found && external.is_function) {
      write_trampoline(image.data() + slot.offset_in_image, external.address);
    } else {
      write_trampoline(image.data() + slot.offset_in_image, 0);
      if (external.found) {
        data_resolved_slots.push_back(slot.symbol_index);
      } else {
        out.pending_fixups.push_back({sym.name, generation_symbol_kind::function,
                                      generation_fixup_kind::function_trampoline,
                                      slot.offset_in_image, 0});
      }
    }
  }
  const auto slot_is_data = [&data_resolved_slots](std::uint32_t symbol_index) {
    return std::find(data_resolved_slots.begin(), data_resolved_slots.end(), symbol_index) !=
           data_resolved_slots.end();
  };

  // Bind mutable state before relocation: every data reference resolves to
  // the storage chosen here. COFF symbols carry no extents, so a symbol's
  // size is the distance to its next sibling in the same section (the
  // linker's view), and its alignment is the section's. Common symbols are
  // the exception: their whole record is a size, with no section at all.
  struct mapped_state {
    std::uintptr_t address = 0;
    std::uint64_t size = 0;
  };
  std::vector<std::optional<mapped_state>> state_by_symbol(obj.symbols.size());
  std::vector<std::vector<std::uint32_t>> symbols_by_section(obj.sections.size() + 1);
  for (std::uint32_t index = 0; index < obj.symbols.size(); ++index) {
    const auto& sym = obj.symbols[index];
    if (sym.auxiliary) {
      continue;
    }
    if (sym.is_common()) {
      symbols_by_section.back().push_back(index);
      continue;
    }
    if (sym.section_number >= 1) {
      const auto& home = obj.sections[static_cast<std::size_t>(sym.section_number) - 1];
      if (home.cls == section_class::data && sym.name != home.name) {
        // The section symbol marks the section; it owns no storage.
        symbols_by_section[static_cast<std::size_t>(sym.section_number) - 1].push_back(index);
      }
    }
  }
  const auto bind_state = [&](std::uint32_t index, std::uint64_t extent, std::uint64_t alignment,
                              const section* home) {
    const auto& sym = obj.symbols[index];
    const auto identity = state_identity(sym, source_path);
    if (identity) {
      if (const auto* committed = find_committed_state(*identity)) {
        if (committed->size != extent || committed->alignment != alignment) {
          throw std::runtime_error(
              "global '" + sym.name + "' changed layout (size " + std::to_string(committed->size) +
              " -> " + std::to_string(extent) + ", alignment " +
              std::to_string(committed->alignment) + " -> " + std::to_string(alignment) +
              ") — changing the layout of existing globals is not supported yet");
        }
        state_by_symbol[index] = mapped_state{committed->address, committed->size};
        return;
      }
    }

    if (void* existing = state_.map_global(sym.name)) {
      if (auto old = symbols_.global_by_name(sym.name);
          old && old->size != 0 && extent > old->size) {
        throw std::runtime_error(
            "global '" + sym.name + "' changed size (" + std::to_string(old->size) + " -> " +
            std::to_string(extent) +
            ") — changing the layout of existing globals is not supported yet");
      }
      state_by_symbol[index] = mapped_state{reinterpret_cast<std::uintptr_t>(existing), extent};
      return;
    }

    if (!identity) {
      throw std::runtime_error("fresh file-local mutable symbol '" + sym.name +
                               "' has no source identity — use a managed reload group or "
                               "watch(object, source)");
    }
    if (alignment > kMaxSectionAlign) {
      throw std::runtime_error("mutable symbol '" + sym.name + "' requires alignment " +
                               std::to_string(alignment) +
                               " — over-aligned globals beyond a page are not supported yet");
    }
    auto state_allocation = substituter_.reserve_writable_near(hint, extent);
    if (state_allocation == nullptr) {
      throw std::runtime_error("could not reserve writable memory near the target for global '" +
                               sym.name + "'");
    }
    if (home == nullptr || home->bytes.empty()) {
      std::memset(state_allocation->data(), 0, static_cast<std::size_t>(extent));
    } else {
      std::memcpy(state_allocation->data(), home->bytes.data() + sym.value,
                  static_cast<std::size_t>(extent));
    }
    const auto address = reinterpret_cast<std::uintptr_t>(state_allocation->data());
    state_by_symbol[index] = mapped_state{address, extent};
    out.state_definitions.push_back({*identity, sym.name, address, extent, alignment, true});
    out.state_allocations.push_back(std::move(state_allocation));
  };
  for (std::size_t group = 0; group < symbols_by_section.size(); ++group) {
    auto& members = symbols_by_section[group];
    const section* home = group + 1 == symbols_by_section.size() ? nullptr : &obj.sections[group];
    if (home == nullptr) {
      // Common symbols: one record each, alignment unknown — 8 is the ABI
      // floor for scalars and small aggregates.
      for (const std::uint32_t index : members) {
        bind_state(index, obj.symbols[index].value, 8, nullptr);
      }
      continue;
    }
    std::sort(members.begin(), members.end(), [&](std::uint32_t left, std::uint32_t right) {
      return obj.symbols[left].value < obj.symbols[right].value;
    });
    for (std::size_t at = 0; at < members.size(); ++at) {
      const std::uint32_t index = members[at];
      const auto& sym = obj.symbols[index];
      if (sym.storage_class != 2 && sym.storage_class != 3) {
        continue; // labels, section symbols, debug records
      }
      const std::uint64_t begin = sym.value;
      const std::uint64_t end =
          at + 1 < members.size()
              ? std::max<std::uint64_t>(begin, obj.symbols[members[at + 1]].value)
              : home->size;
      const std::uint64_t extent = end - begin;
      if (extent == 0 || begin > home->size || extent > home->size - begin) {
        throw std::runtime_error("mutable symbol '" + sym.name +
                                 "' has an invalid or unknown storage extent");
      }
      bind_state(index, extent, home->align, home);
    }
  }

  // Generation-visible definitions: functions from the plan, objects from
  // the bound state table.
  for (const auto& candidate : plan.functions) {
    const auto& sym = obj.symbols[candidate.symbol_index];
    if (sym.storage_class == 2) {
      out.exported_symbols.push_back(
          {sym.name, generation_symbol_kind::function, candidate.offset_in_image, 0});
    }
  }
  for (std::uint32_t index = 0; index < obj.symbols.size(); ++index) {
    const auto& sym = obj.symbols[index];
    if (sym.auxiliary || sym.storage_class != 2 || !state_by_symbol[index]) {
      continue;
    }
    out.exported_symbols.push_back(
        {sym.name, generation_symbol_kind::object, 0, state_by_symbol[index]->address});
  }

  // Apply every relocation whose site is placed. Sites in sections that
  // never enter the image (debug streams) are inert and skipped.
  for (const auto& rel : obj.relocations) {
    if (rel.target_section >= obj.sections.size()) {
      continue;
    }
    const auto site_offset = placement_of[rel.target_section];
    if (site_offset == kUnplaced) {
      continue; // never copied: debug streams and their kin
    }
    const auto& target_sec = obj.sections[rel.target_section];
    if (rel.symbol_index >= obj.symbols.size()) {
      throw std::runtime_error("relocation with a symbol index out of bounds");
    }
    const auto& sym = obj.symbols[rel.symbol_index];
    // Relocated constants refuse — vtables, jump tables and friends would
    // crash at first use, not at load — with one exception: an absolute_64
    // merely materializes an address (MSVC's exception metadata chains
    // .rdata and .data$r through such pointers), which the image layout
    // satisfies.
    if (target_sec.cls == section_class::rodata && !unwind_section(target_sec) &&
        rel.kind != relocation_kind::absolute_64) {
      throw std::runtime_error("section '" + target_sec.name +
                               "' carries relocations — relocated constant tables (vtables, "
                               "jump tables) are not supported yet");
    }
    if (rel.kind == relocation_kind::section_relative_32 ||
        rel.kind == relocation_kind::section_index_16 || rel.kind == relocation_kind::unsupported) {
      throw std::runtime_error("relocation type is not supported inside the code image (symbol '" +
                               sym.name + "')");
    }

    const std::uint64_t write_at = site_offset + rel.offset;
    const auto width = rel.kind == relocation_kind::absolute_64 ? 8u : 4u;
    if (write_at > image.size() || width > image.size() - write_at) {
      throw std::runtime_error("relocation for '" + sym.name + "' falls outside the code image");
    }
    const std::uintptr_t p = base + write_at;
    std::uint8_t* site = image.data() + write_at;

    // Calls reach their target through the trampoline slot; the stored
    // addend at a call site is zero by construction. A slot resolved as
    // data falls through: its sites take the direct address, since a load
    // cannot cross a jump thunk.
    if (rel.kind == relocation_kind::relative_32) {
      const auto slot = slot_for_symbol.find(rel.symbol_index);
      if (slot != slot_for_symbol.end() && !slot_is_data(rel.symbol_index)) {
        const std::int64_t value =
            static_cast<std::int64_t>(base + slot->second) - static_cast<std::int64_t>(p + 4);
        std::int32_t narrow = static_cast<std::int32_t>(value);
        std::memcpy(site, &narrow, sizeof(narrow));
        continue;
      }
    }

    // Bound state answers first — data-section definitions and common
    // records alike; everything else resolves through the process, the
    // image, or a pending sibling fixup.
    std::uintptr_t s = 0;
    bool bound_to_state = false;
    if (state_by_symbol[rel.symbol_index]) {
      s = state_by_symbol[rel.symbol_index]->address;
      bound_to_state = true;
    } else if (sym.section_number >= 1 &&
               obj.sections[static_cast<std::size_t>(sym.section_number) - 1].cls ==
                   section_class::data) {
      throw std::runtime_error("cannot map mutable symbol '" + sym.name + "' onto live state");
    } else if (sym.section_number == 0) {
      const auto external = resolve_external(symbols_, sym);
      if (!external.found) {
        // A generation sibling may define this name; leave a typed fixup.
        std::optional<generation_fixup_kind> pending_kind;
        if (rel.kind == relocation_kind::absolute_64) {
          pending_kind = generation_fixup_kind::absolute_64;
        } else if (rel.kind == relocation_kind::absolute_32) {
          pending_kind = generation_fixup_kind::absolute_32;
        }
        if (!pending_kind) {
          throw std::runtime_error("cannot resolve external symbol '" + sym.name +
                                   "' — the process has no link-visible definition and the "
                                   "relocation has no pending form");
        }
        const auto target_kind =
            sym.is_function() ? generation_symbol_kind::function : generation_symbol_kind::object;
        out.pending_fixups.push_back({sym.name, target_kind, *pending_kind,
                                      static_cast<std::uint32_t>(write_at),
                                      static_cast<std::int64_t>(read_stored32(site))});
        std::memset(site, 0, width);
        continue;
      }
      s = external.address;
    } else {
      if (sym.section_number < 0 ||
          static_cast<std::size_t>(sym.section_number) > obj.sections.size()) {
        throw std::runtime_error("symbol '" + sym.name + "' has an invalid section number");
      }
      if (sym.is_common()) {
        throw std::runtime_error("common symbol '" + sym.name +
                                 "' needs fresh storage — unresolved at relocation time");
      }
      const auto& sec = obj.sections[static_cast<std::size_t>(sym.section_number) - 1];
      const auto offset = placement_of[static_cast<std::size_t>(sym.section_number) - 1];
      if (offset == kUnplaced) {
        throw std::runtime_error("symbol '" + sym.name + "' lives in section '" + sec.name +
                                 "', which is not part of the code image");
      }
      s = base + offset + sym.value;
    }

    const std::int64_t stored =
        rel.kind == relocation_kind::absolute_64 ? read_stored64(site) : read_stored32(site);
    std::int64_t value = 0;
    switch (rel.kind) {
    case relocation_kind::relative_32:
      value =
          static_cast<std::int64_t>(s) + stored - static_cast<std::int64_t>(p + 4 + rel.rel32_bias);
      break;
    case relocation_kind::absolute_64:
      value = static_cast<std::int64_t>(s) + stored;
      break;
    case relocation_kind::absolute_32:
      value = static_cast<std::int64_t>(s) + stored;
      break;
    case relocation_kind::image_relative_32:
      value = bound_to_state
                  ? static_cast<std::int64_t>(s) + stored
                  : static_cast<std::int64_t>(s) + stored - static_cast<std::int64_t>(base);
      break;
    default:
      throw std::runtime_error("relocation type is not supported inside the code image");
    }
    if (rel.kind != relocation_kind::absolute_64 &&
        (value > 0x7FFF'FFFF || value < -0x8000'0000LL)) {
      throw std::runtime_error("relocation out of 32-bit range for '" + sym.name + "'");
    }
    if (rel.kind == relocation_kind::absolute_64) {
      std::memcpy(site, &value, sizeof(std::int64_t));
    } else {
      std::int32_t narrow = static_cast<std::int32_t>(value);
      std::memcpy(site, &narrow, sizeof(narrow));
    }
  }

  // Live entries to redirect.
  std::vector<std::string> not_redirected;
  for (const auto& candidate : plan.functions) {
    const auto& sym = obj.symbols[candidate.symbol_index];

    std::optional<backend::function_info> old;
    const auto duplicates = symbols_.count_functions(sym.name);
    if (duplicates > 1) {
      throw std::runtime_error(
          "ambiguous function '" + sym.name + "' (" + std::to_string(duplicates) +
          " symbols share the name) — refusing to patch by name; the compiland manifest that "
          "would identify the translation unit is not implemented yet");
    }
    if (sym.storage_class != 2) {
      // A static name is local to a translation unit; matching it by name
      // alone could redirect another unit's function. Without the manifest
      // that proof does not exist, so refuse rather than guess.
      if (symbols_.function_by_name(sym.name).has_value()) {
        throw std::runtime_error(
            "file-static function '" + sym.name +
            "' shares its name with a live function — refusing to patch by name without the "
            "compiland manifest");
      }
    } else {
      old = symbols_.function_by_name(sym.name);
    }
    if (!old) {
      // A fresh helper or a new function: it lives in the arena and
      // redirects nothing.
      not_redirected.push_back(sym.name);
      continue;
    }
    if (old->size != 0 && old->size < 8) {
      throw std::runtime_error("function '" + sym.name + "' is too small to patch safely (" +
                               std::to_string(old->size) + " bytes)");
    }
    function_replacement repl;
    repl.name = sym.name;
    repl.old_entry = old->address;
    repl.offset_in_image = candidate.offset_in_image;
    out.replacements.push_back(std::move(repl));
  }

  if (!substituter_.commit_code(*out.allocation, image.data(), image.size())) {
    throw std::runtime_error("failed to commit code image");
  }

  // Register the unwind tail with the process. The relocated .pdata holds
  // image-relative RVAs, exactly what RtlAddFunctionTable wants against
  // this image's base; the registration lives as long as the allocation,
  // and release_to_process keeps it for the process lifetime.
  for (const auto& p : plan.unwind) {
    if (obj.sections[p.section].name != ".pdata" || p.size == 0) {
      continue;
    }
    auto* table =
        reinterpret_cast<RUNTIME_FUNCTION*>(static_cast<std::uint8_t*>(out.code()) + p.offset);
    const DWORD count = static_cast<DWORD>(p.size / 12);
    if (RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(out.code())) == FALSE) {
      throw std::runtime_error("could not register the unwind tables of the fresh image");
    }
    substituter_.on_reclaim(
        *out.allocation,
        [](void* context) { RtlDeleteFunctionTable(static_cast<RUNTIME_FUNCTION*>(context)); },
        table);
    break; // one .pdata table per image covers every entry
  }

  for (const auto& repl : out.replacements) {
    neko::log(
        neko::log_level::info, "  %s -> %p (+0x%x)\n", repl.name.c_str(),
        static_cast<void*>(reinterpret_cast<std::uint8_t*>(out.code()) + repl.offset_in_image),
        repl.offset_in_image);
  }
  for (const auto& name : not_redirected) {
    neko::log(neko::log_level::info,
              "not redirected: '%s' — no live entry to replace; the fresh body stays in the "
              "arena\n",
              name.c_str());
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
                                 "' — the process has no link-visible definition and the "
                                 "generation does not define one either");
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

} // namespace neko::pe
