#include "inspect.hpp"
#include "../elf/binary_file.hpp"

#include <dwarf.h>
#include <libdwarf.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace neko::dwarf {
namespace {

struct debug_deleter {
  void operator()(Dwarf_Debug value) const { dwarf_finish(value); }
};
struct die_deleter {
  void operator()(Dwarf_Die value) const { dwarf_dealloc_die(value); }
};
struct attribute_deleter {
  void operator()(Dwarf_Attribute value) const { dwarf_dealloc_attribute(value); }
};
struct error_deleter {
  Dwarf_Debug debug;
  void operator()(Dwarf_Error value) const { dwarf_dealloc_error(debug, value); }
};
using debug_ptr = std::unique_ptr<std::remove_pointer_t<Dwarf_Debug>, debug_deleter>;
using die_ptr = std::unique_ptr<std::remove_pointer_t<Dwarf_Die>, die_deleter>;
using attribute_ptr = std::unique_ptr<std::remove_pointer_t<Dwarf_Attribute>, attribute_deleter>;

template <typename Call>
bool checked_call(Dwarf_Debug debug, std::string_view operation, Call&& call) {
  Dwarf_Error error = nullptr;
  const int status = call(&error);
  const std::unique_ptr<std::remove_pointer_t<Dwarf_Error>, error_deleter> owner(error, {debug});
  if (status == DW_DLV_OK) {
    return true;
  }
  if (status == DW_DLV_NO_ENTRY) {
    return false;
  }
  throw std::runtime_error(std::string(operation) + ": " +
                           (error ? dwarf_errmsg(error) : "libdwarf error without details"));
}

class reader {
public:
  explicit reader(const elf::binary_file& file)
      : file_(file), executable_(file.executable_ranges()) {
    Dwarf_Debug debug = nullptr;
    const bool found = checked_call(nullptr, "open DWARF", [&](Dwarf_Error* error) {
      // Same descriptor as the ELF check. No debuglink/dSYM lookup.
      return dwarf_init_b(file_.fd(), DW_GROUPNUMBER_BASE, nullptr, nullptr, &debug, error);
    });
    debug_.reset(debug);
    if (!found) {
      throw std::runtime_error("no embedded DWARF information; rebuild with -g -gdwarf-4");
    }
  }

  std::vector<compilation_unit> read() {
    std::vector<compilation_unit> units;
    while (true) {
      Dwarf_Die raw = nullptr;
      Dwarf_Unsigned length = 0, type_offset = 0, next_offset = 0;
      Dwarf_Off abbrev = 0;
      Dwarf_Half version = 0, address_size = 0, length_size = 0, extension_size = 0, unit_type = 0;
      Dwarf_Sig8 signature{};
      const bool found = call("read compilation unit", [&](Dwarf_Error* error) {
        return dwarf_next_cu_header_e(debug_.get(), true, &raw, &length, &version, &abbrev,
                                      &address_size, &length_size, &extension_size, &signature,
                                      &type_offset, &next_offset, &unit_type, error);
      });
      die_ptr die(raw);
      if (!found) {
        break;
      }
      if (version != 4 || address_size != 8 || length_size != 4 || extension_size != 0 ||
          unit_type != DW_UT_compile || tag(die.get()) != DW_TAG_compile_unit) {
        throw std::runtime_error("unsupported compilation unit; require DWARF 4, DWARF32, "
                                 "8-byte addresses and ordinary compile units");
      }
      if (attribute(die.get(), DW_AT_GNU_dwo_name) || attribute(die.get(), DW_AT_dwo_name)) {
        throw std::runtime_error("split DWARF is unsupported");
      }
      compilation_unit unit;
      unit.die_offset = die_offset(die.get());
      unit.name = string_attribute(die.get(), DW_AT_name).value_or("");
      unit.directory = string_attribute(die.get(), DW_AT_comp_dir).value_or("");
      unit.producer = string_attribute(die.get(), DW_AT_producer).value_or("");
      if (unit.name.empty()) {
        throw std::runtime_error("compilation unit has no source name");
      }
      walk(die.get(), unit, 0);
      units.push_back(std::move(unit));
    }
    if (units.empty()) {
      throw std::runtime_error("no embedded DWARF compilation units");
    }
    return units;
  }

private:
  template <typename Call>
  bool call(std::string_view operation, Call&& action) const {
    return checked_call(debug_.get(), operation, std::forward<Call>(action));
  }

  attribute_ptr attribute(Dwarf_Die die, Dwarf_Half kind) const {
    Dwarf_Attribute raw = nullptr;
    call("read attribute", [&](Dwarf_Error* error) { return dwarf_attr(die, kind, &raw, error); });
    return attribute_ptr(raw);
  }

  std::optional<std::string> string_attribute(Dwarf_Die die, Dwarf_Half kind) const {
    const auto attr = attribute(die, kind);
    if (!attr) {
      return std::nullopt;
    }
    char* value = nullptr;
    if (!call("read string",
              [&](Dwarf_Error* error) { return dwarf_formstring(attr.get(), &value, error); }) ||
        value == nullptr) {
      throw std::runtime_error("missing DWARF string value");
    }
    return std::string(value);
  }

  std::optional<std::string> inherited_string(Dwarf_Die die, Dwarf_Half kind,
                                              unsigned depth = 0) const {
    if (depth > 32) {
      throw std::runtime_error("cyclic or excessively deep DWARF name reference");
    }
    if (auto value = string_attribute(die, kind)) {
      return value;
    }
    for (const auto reference : {DW_AT_specification, DW_AT_abstract_origin}) {
      const auto attr = attribute(die, reference);
      if (!attr) {
        continue;
      }
      Dwarf_Off offset = 0;
      Dwarf_Bool is_info = false;
      if (!call("read name reference",
                [&](Dwarf_Error* error) {
                  return dwarf_global_formref_b(attr.get(), &offset, &is_info, error);
                }) ||
          !is_info) {
        throw std::runtime_error("unsupported DWARF name reference");
      }
      Dwarf_Die raw = nullptr;
      const bool found = call("follow name reference", [&](Dwarf_Error* error) {
        return dwarf_offdie_b(debug_.get(), offset, true, &raw, error);
      });
      const die_ptr origin(raw);
      if (!found) {
        throw std::runtime_error("dangling DWARF name reference");
      }
      if (auto value = inherited_string(origin.get(), kind, depth + 1)) {
        return value;
      }
    }
    return std::nullopt;
  }

  Dwarf_Half tag(Dwarf_Die die) const {
    Dwarf_Half value = 0;
    if (!call("read DIE tag", [&](Dwarf_Error* error) { return dwarf_tag(die, &value, error); })) {
      throw std::runtime_error("missing DIE tag");
    }
    return value;
  }

  std::uint64_t die_offset(Dwarf_Die die) const {
    Dwarf_Off value = 0;
    if (!call("read DIE offset",
              [&](Dwarf_Error* error) { return dwarf_dieoffset(die, &value, error); })) {
      throw std::runtime_error("missing DIE offset");
    }
    return value;
  }

  void read_function(Dwarf_Die die, compilation_unit& unit) const {
    if (const auto attr = attribute(die, DW_AT_declaration)) {
      Dwarf_Bool declaration = false;
      call("read declaration flag",
           [&](Dwarf_Error* error) { return dwarf_formflag(attr.get(), &declaration, error); });
      if (declaration) {
        return;
      }
    }
    if (attribute(die, DW_AT_ranges)) {
      throw std::runtime_error("function range lists are unsupported in this inspector");
    }
    function_record function;
    function.die_offset = die_offset(die);
    function.name = inherited_string(die, DW_AT_name).value_or("");
    function.linkage_name = inherited_string(die, DW_AT_linkage_name);
    if (!function.linkage_name) {
      function.linkage_name = inherited_string(die, DW_AT_MIPS_linkage_name);
    }
    Dwarf_Addr low = 0, high = 0;
    Dwarf_Half form = 0;
    Dwarf_Form_Class form_class = DW_FORM_CLASS_UNKNOWN;
    const bool has_low =
        call("read low_pc", [&](Dwarf_Error* error) { return dwarf_lowpc(die, &low, error); });
    const bool has_high = call("read high_pc", [&](Dwarf_Error* error) {
      return dwarf_highpc_b(die, &high, &form, &form_class, error);
    });
    if (!has_low && !has_high) {
      unit.unlocated_functions.push_back(function.name.empty() ? "<unnamed>" : function.name);
      return;
    }
    if (!has_low || !has_high) {
      throw std::runtime_error("incomplete function code range");
    }
    if (form_class == DW_FORM_CLASS_CONSTANT) {
      if (high > std::numeric_limits<std::uint64_t>::max() - low) {
        throw std::runtime_error("function high_pc overflow");
      }
      high += low;
    } else if (form_class != DW_FORM_CLASS_ADDRESS) {
      throw std::runtime_error("unsupported high_pc form");
    }
    if (low == 0 || high <= low ||
        !std::any_of(executable_.begin(), executable_.end(),
                     [&](const auto& range) { return low >= range.begin && high <= range.end; })) {
      throw std::runtime_error("function code range is empty or outside executable segments");
    }
    if (function.name.empty()) {
      throw std::runtime_error("function with code has no source name");
    }
    function.ranges.push_back({low, high});
    unit.functions.push_back(std::move(function));
  }

  void walk(Dwarf_Die parent, compilation_unit& unit, unsigned depth) {
    if (depth > 128) {
      throw std::runtime_error("DWARF tree exceeds inspection depth limit");
    }
    Dwarf_Die raw = nullptr;
    call("read child DIE", [&](Dwarf_Error* error) { return dwarf_child(parent, &raw, error); });
    die_ptr die(raw);
    while (die) {
      if (!visited_.insert(die_offset(die.get())).second || visited_.size() > 1000000) {
        throw std::runtime_error("cyclic or excessively large DWARF tree");
      }
      const auto kind = tag(die.get());
      if (kind == DW_TAG_subprogram) {
        read_function(die.get(), unit);
      } else if (kind == DW_TAG_inlined_subroutine) {
        throw std::runtime_error("inlined subroutines are unsupported; use an -O0 build");
      }
      walk(die.get(), unit, depth + 1);
      Dwarf_Die sibling = nullptr;
      call("read sibling DIE",
           [&](Dwarf_Error* error) { return dwarf_siblingof_c(die.get(), &sibling, error); });
      die.reset(sibling);
    }
  }

  const elf::binary_file& file_;
  std::vector<elf::address_range> executable_;
  debug_ptr debug_; // the caller keeps the descriptor alive until inspection finishes
  std::unordered_set<std::uint64_t> visited_;
};

} // namespace

binary_info inspect(const std::filesystem::path& path) {
  return inspect(elf::binary_file(path));
}

binary_info inspect(const elf::binary_file& file) {
  reader input(file);
  return {file.path(), input.read()};
}

} // namespace neko::dwarf
