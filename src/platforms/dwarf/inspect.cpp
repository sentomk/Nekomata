#include "inspect.hpp"
#include "../elf/binary_file.hpp"

#include <dwarf.h>
#include <elf.h>
#include <libdwarf.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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

constexpr std::size_t kStringBudget = 64 * 1024 * 1024;
constexpr std::uint64_t kFileEntryBudget = 1000000;
constexpr std::uint64_t kLookupBudget = 8000000;

struct inherited_value {
  attribute_ptr attribute;
  std::uint64_t unit = 0; // File indexes belong to the attribute's own CU.
};

struct source_table {
  // Views point into header, which moves with this table and is never resized.
  std::vector<std::uint8_t> header;
  std::string directory;
  std::vector<std::string_view> directories;
  std::vector<std::pair<std::string_view, std::uint64_t>> files;
  std::unordered_map<std::uint64_t, std::string> paths;
};

class source_cursor {
public:
  explicit source_cursor(std::span<const std::uint8_t> data) : data_(data) {}

  std::uint64_t number(std::size_t width) {
    const auto bytes = take(width);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
  }

  std::uint64_t leb() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift <= 63; shift += 7) {
      const auto byte = number(1);
      if (shift == 63 && byte > 1) {
        break;
      }
      value |= (byte & 0x7f) << shift;
      if (!(byte & 0x80)) {
        return value;
      }
    }
    throw std::runtime_error("overflow in source table ULEB128");
  }

  std::string_view string() {
    const auto end = std::find(data_.begin(), data_.end(), std::uint8_t{0});
    if (end == data_.end()) {
      throw std::runtime_error("unterminated source table string");
    }
    const auto size = static_cast<std::size_t>(end - data_.begin());
    const auto bytes = take(size + 1);
    return {reinterpret_cast<const char*>(bytes.data()), size};
  }

  std::span<const std::uint8_t> take(std::size_t count) {
    if (count > data_.size()) {
      throw std::runtime_error("truncated source table header");
    }
    const auto result = data_.first(count);
    data_ = data_.subspan(count);
    return result;
  }

  bool empty() const { return data_.empty(); }

private:
  std::span<const std::uint8_t> data_;
};

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
  std::string copy_string(std::string_view value) const {
    if (value.size() > kStringBudget - string_bytes_) {
      throw std::runtime_error("DWARF strings exceed inspection byte limit");
    }
    string_bytes_ += value.size();
    return std::string(value);
  }

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
    return copy_string(value);
  }

  inherited_value inherited_attribute(Dwarf_Die die, Dwarf_Half kind, unsigned depth = 0) const {
    if (depth > 32) {
      throw std::runtime_error("cyclic or excessively deep DWARF attribute reference");
    }
    if (auto value = attribute(die, kind)) {
      Dwarf_Off unit = 0;
      if (!call("read attribute compilation unit", [&](Dwarf_Error* error) {
            return dwarf_CU_dieoffset_given_die(die, &unit, error);
          })) {
        throw std::runtime_error("missing attribute compilation unit");
      }
      return {std::move(value), unit};
    }
    for (const auto reference : {DW_AT_specification, DW_AT_abstract_origin}) {
      const auto attr = attribute(die, reference);
      if (!attr) {
        continue;
      }
      if (++reference_steps_ > kLookupBudget) {
        throw std::runtime_error("DWARF attribute references exceed inspection work limit");
      }
      Dwarf_Off offset = 0;
      Dwarf_Bool is_info = false;
      if (!call("read attribute reference",
                [&](Dwarf_Error* error) {
                  return dwarf_global_formref_b(attr.get(), &offset, &is_info, error);
                }) ||
          !is_info) {
        throw std::runtime_error("unsupported DWARF attribute reference");
      }
      Dwarf_Die raw = nullptr;
      const bool found = call("follow attribute reference", [&](Dwarf_Error* error) {
        return dwarf_offdie_b(debug_.get(), offset, true, &raw, error);
      });
      const die_ptr origin(raw);
      if (!found) {
        throw std::runtime_error("dangling DWARF attribute reference");
      }
      if (auto value = inherited_attribute(origin.get(), kind, depth + 1); value.attribute) {
        return value;
      }
    }
    return {};
  }

  std::optional<std::string> inherited_string(Dwarf_Die die, Dwarf_Half kind) const {
    const auto value = inherited_attribute(die, kind);
    if (!value.attribute) {
      return std::nullopt;
    }
    char* text = nullptr;
    if (!call("read inherited string",
              [&](Dwarf_Error* error) {
                return dwarf_formstring(value.attribute.get(), &text, error);
              }) ||
        !text) {
      throw std::runtime_error("missing DWARF string value");
    }
    return copy_string(text);
  }

  std::uint64_t unsigned_value(Dwarf_Attribute attr) const {
    Dwarf_Half form = 0;
    if (!call("read coordinate form",
              [&](Dwarf_Error* error) { return dwarf_whatform(attr, &form, error); })) {
      throw std::runtime_error("missing declaration coordinate form");
    }
    if (form == DW_FORM_sdata) {
      Dwarf_Signed value = 0;
      if (!call("read signed coordinate",
                [&](Dwarf_Error* error) { return dwarf_formsdata(attr, &value, error); }) ||
          value < 0) {
        throw std::runtime_error("negative or missing declaration coordinate");
      }
      return static_cast<std::uint64_t>(value);
    }
    if (form != DW_FORM_data1 && form != DW_FORM_data2 && form != DW_FORM_data4 &&
        form != DW_FORM_data8 && form != DW_FORM_udata) {
      throw std::runtime_error("unsupported declaration coordinate form");
    }
    Dwarf_Unsigned value = 0;
    if (!call("read declaration coordinate",
              [&](Dwarf_Error* error) { return dwarf_formudata(attr, &value, error); })) {
      throw std::runtime_error("missing declaration coordinate");
    }
    return value;
  }

  source_table& source_files(std::uint64_t offset) {
    if (const auto found = source_tables_.find(offset); found != source_tables_.end()) {
      return found->second;
    }
    Dwarf_Die raw = nullptr;
    const bool found = call("read source compilation unit", [&](Dwarf_Error* error) {
      return dwarf_offdie_b(debug_.get(), offset, true, &raw, error);
    });
    die_ptr unit(raw);
    if (!found || tag(unit.get()) != DW_TAG_compile_unit) {
      throw std::runtime_error("invalid source compilation unit");
    }
    source_table table;
    if (const auto stmt = attribute(unit.get(), DW_AT_stmt_list)) {
      table.header = source_header(stmt.get());
      if (table.header.empty()) {
        return source_tables_.emplace(offset, std::move(table)).first->second;
      }
      table.directory = string_attribute(unit.get(), DW_AT_comp_dir).value_or("");
      source_cursor cursor(table.header);
      const auto instruction_size = cursor.number(1);
      const auto operations = cursor.number(1);
      const auto default_statement = cursor.number(1);
      cursor.take(1); // signed line_base is irrelevant without interpreting rows.
      const auto line_range = cursor.number(1);
      const auto opcode_base = cursor.number(1);
      if (!instruction_size || !operations || default_statement > 1 || !line_range ||
          !opcode_base) {
        throw std::runtime_error("invalid source table parameters");
      }
      cursor.take(static_cast<std::size_t>(opcode_base - 1));
      for (auto directory = cursor.string(); !directory.empty(); directory = cursor.string()) {
        source_entry();
        table.directories.push_back(directory);
      }
      for (auto name = cursor.string(); !name.empty(); name = cursor.string()) {
        source_entry();
        const auto directory = cursor.leb();
        cursor.leb(); // modification time
        cursor.leb(); // file size
        if (directory > table.directories.size()) {
          throw std::runtime_error("invalid source directory index");
        }
        table.files.emplace_back(name, directory);
      }
      if (!cursor.empty()) {
        throw std::runtime_error("unexpected data in source table header");
      }
    }
    return source_tables_.emplace(offset, std::move(table)).first->second;
  }

  void source_entry() {
    if (++file_entries_ > kFileEntryBudget) {
      throw std::runtime_error("DWARF source tables exceed inspection entry limit");
    }
  }

  std::vector<std::uint8_t> source_header(Dwarf_Attribute stmt) {
    Dwarf_Half form = 0;
    Dwarf_Off offset = 0;
    if (!call("read statement list form",
              [&](Dwarf_Error* error) { return dwarf_whatform(stmt, &form, error); }) ||
        form != DW_FORM_sec_offset || !call("read statement list offset", [&](Dwarf_Error* error) {
          return dwarf_global_formref(stmt, &offset, error);
        })) {
      throw std::runtime_error("unsupported statement list offset");
    }
    if (!line_section_loaded_) {
      Dwarf_Unsigned flags = 0;
      const bool found = call("locate source section", [&](Dwarf_Error* error) {
        return dwarf_get_section_info_by_name_a(debug_.get(), ".debug_line", nullptr, &line_size_,
                                                &flags, &line_offset_, error);
      });
      if (!found && call("locate compressed source section", [&](Dwarf_Error* error) {
            return dwarf_get_section_info_by_name_a(debug_.get(), ".zdebug_line", nullptr, nullptr,
                                                    nullptr, nullptr, error);
          })) {
        throw std::runtime_error("compressed source section is unsupported");
      }
      if (found && ((flags & SHF_COMPRESSED) || !file_.contains(line_offset_, line_size_))) {
        throw std::runtime_error("compressed or invalid source section");
      }
      line_section_loaded_ = true;
    }
    if (line_size_ == 0) {
      return {}; // Stripped/missing line metadata does not invalidate other coordinates.
    }
    if (offset > line_size_ || line_size_ - offset < 10) {
      throw std::runtime_error("truncated source table prefix");
    }
    std::array<std::uint8_t, 10> prefix{};
    file_.read(line_offset_ + offset, prefix);
    source_cursor cursor(prefix);
    const auto length = cursor.number(4);
    const auto version = cursor.number(2);
    const auto header_length = cursor.number(4);
    if (length >= 0xfffffff0 || version != 4) {
      throw std::runtime_error("unsupported source file table; require DWARF 4 and DWARF32");
    }
    if (length < 6 || length > line_size_ - offset - 4 || header_length < 8 ||
        header_length > length - 6) {
      throw std::runtime_error("invalid source table length");
    }
    // libdwarf 2.3.2's dwarf_srclines_b expands the full row program. Read only
    // the bounded DWARF 4 header here; DIEs/attributes still use libdwarf.
    if (header_length > kStringBudget - source_header_bytes_) {
      throw std::runtime_error("DWARF source headers exceed inspection byte limit");
    }
    source_header_bytes_ += static_cast<std::size_t>(header_length);
    std::vector<std::uint8_t> header(static_cast<std::size_t>(header_length));
    file_.read(line_offset_ + offset + 10, header);
    return header;
  }

  std::string join_path(std::string_view directory, std::string_view file) const {
    if (directory.empty() || file.starts_with('/')) {
      return copy_string(file);
    }
    // Keep recorded components, including '..': resolving symlinks or relative
    // paths against the inspector's current directory would invent provenance.
    const std::size_t separator = directory.ends_with('/') ? 0 : 1;
    if (directory.size() > kStringBudget - string_bytes_ ||
        separator > kStringBudget - string_bytes_ - directory.size() ||
        file.size() > kStringBudget - string_bytes_ - directory.size() - separator) {
      throw std::runtime_error("DWARF strings exceed inspection byte limit");
    }
    string_bytes_ += directory.size() + separator + file.size();
    std::string result(directory);
    if (separator) {
      result += '/';
    }
    result += file;
    return result;
  }

  std::optional<std::string> source_file(std::uint64_t unit, std::uint64_t index) {
    auto& table = source_files(unit);
    if (table.header.empty()) {
      return std::nullopt; // No line table: do not substitute the CU's filename.
    }
    if (index > table.files.size()) {
      throw std::runtime_error("declaration file index is outside the source file table");
    }
    if (const auto found = table.paths.find(index); found != table.paths.end()) {
      return copy_string(found->second);
    }
    const auto& [name, directory] = table.files[static_cast<std::size_t>(index - 1)];
    const auto include =
        directory ? table.directories[static_cast<std::size_t>(directory - 1)] : std::string_view{};
    auto path = join_path(include, name);
    path = join_path(table.directory, path);
    auto result = copy_string(path);
    table.paths.emplace(index, std::move(path));
    return result;
  }

  source_location declaration(Dwarf_Die die) {
    source_location location;
    if (const auto value = inherited_attribute(die, DW_AT_decl_file); value.attribute) {
      if (const auto index = unsigned_value(value.attribute.get()); index != 0) {
        location.file = source_file(value.unit, index);
      }
    }
    for (const auto kind : {DW_AT_decl_line, DW_AT_decl_column}) {
      if (const auto value = inherited_attribute(die, kind); value.attribute) {
        if (const auto number = unsigned_value(value.attribute.get()); number != 0) {
          (kind == DW_AT_decl_line ? location.line : location.column) = number;
        }
      }
    }
    return location;
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

  void read_function(Dwarf_Die die, compilation_unit& unit) {
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
    function.declaration = declaration(die);
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
  std::unordered_map<std::uint64_t, source_table> source_tables_;
  mutable std::size_t string_bytes_ = 0;
  mutable std::uint64_t reference_steps_ = 0;
  std::uint64_t file_entries_ = 0;
  std::size_t source_header_bytes_ = 0;
  Dwarf_Unsigned line_offset_ = 0, line_size_ = 0;
  bool line_section_loaded_ = false;
  std::unordered_set<std::uint64_t> visited_;
};

} // namespace

binary_info inspect(const std::filesystem::path& path) {
  return inspect(elf::binary_file(path));
}

binary_info inspect(const elf::binary_file& file) {
  if (file.kind() == elf::binary_kind::relocatable) {
    throw std::runtime_error("relocatable DWARF inspection is not yet supported");
  }
  reader input(file);
  return {file.path(), input.read()};
}

} // namespace neko::dwarf
