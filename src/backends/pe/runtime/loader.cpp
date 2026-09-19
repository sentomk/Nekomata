#include "runtime/loader.hpp"

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

/// One live-process resolution of an undefined external. Drivers disagree
/// about the type bit on undefined records (MSVC marks functions, clang-cl
/// does not), so both streams are probed; the hitting stream decides the
/// symbol's shape. Ambiguity (count > 1) refuses rather than guesses.
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

} // namespace

loader::loader(backend::symbol_provider& symbols, backend::code_substituter& substituter)
    : symbols_(symbols), substituter_(substituter) {}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size) {
  return load(object_data, size, {});
}

loaded_image loader::load(const std::uint8_t* object_data, std::size_t size,
                          std::string_view source_path) {
  (void)source_path; // file-static disambiguation needs the compiland manifest
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

  // The resolved address of one symbol, or nullopt when only a generation
  // sibling can define it.
  const auto resolve = [&](const symbol& sym) -> std::optional<std::uintptr_t> {
    if (sym.section_number == 0) {
      const auto external = resolve_external(symbols_, sym);
      if (external.found) {
        return external.address;
      }
      return std::nullopt;
    }
    if (sym.section_number < 0 ||
        static_cast<std::size_t>(sym.section_number) > obj.sections.size()) {
      throw std::runtime_error("symbol '" + sym.name + "' has an invalid section number");
    }
    const auto& sec = obj.sections[static_cast<std::size_t>(sym.section_number) - 1];
    if (sec.cls == section_class::data) {
      throw std::runtime_error("cannot map mutable symbol '" + sym.name +
                               "' onto live state — the state path is not implemented yet");
    }
    if (sym.is_common()) {
      throw std::runtime_error("common symbol '" + sym.name +
                               "' needs fresh storage — the state path is not implemented yet");
    }
    const auto offset = placement_of[static_cast<std::size_t>(sym.section_number) - 1];
    if (offset == kUnplaced) {
      throw std::runtime_error("symbol '" + sym.name + "' lives in section '" + sec.name +
                               "', which is not part of the code image");
    }
    return base + offset + sym.value;
  };

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
    if (target_sec.cls == section_class::rodata && !unwind_section(target_sec)) {
      throw std::runtime_error("section '" + target_sec.name +
                               "' carries relocations — relocated constant tables (vtables, "
                               "jump tables) are not supported yet");
    }
    if (rel.symbol_index >= obj.symbols.size()) {
      throw std::runtime_error("relocation with a symbol index out of bounds");
    }
    const auto& sym = obj.symbols[rel.symbol_index];
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

    auto resolved = resolve(sym);
    if (!resolved) {
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

    const std::int64_t stored =
        rel.kind == relocation_kind::absolute_64 ? read_stored64(site) : read_stored32(site);
    const std::int64_t s = static_cast<std::int64_t>(*resolved);
    std::int64_t value = 0;
    switch (rel.kind) {
    case relocation_kind::relative_32:
      value = s + stored - static_cast<std::int64_t>(p + 4 + rel.rel32_bias);
      break;
    case relocation_kind::absolute_64:
      value = s + stored;
      break;
    case relocation_kind::absolute_32:
      value = s + stored;
      break;
    case relocation_kind::image_relative_32:
      value = s + stored - static_cast<std::int64_t>(base);
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

  // Live entries to redirect, and the generation-visible definitions.
  std::vector<std::string> not_redirected;
  for (const auto& candidate : plan.functions) {
    const auto& sym = obj.symbols[candidate.symbol_index];
    if (sym.storage_class == 2) {
      out.exported_symbols.push_back(
          {sym.name, generation_symbol_kind::function, candidate.offset_in_image, 0});
    }

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

} // namespace neko::pe
