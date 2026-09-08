#include "process_symbols.hpp"

#include <link.h>

#include <elf.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <neko/log.hpp>

namespace neko::elf {
namespace {

/// The main executable's load base: the object whose dlpi_name is empty.
/// For a PIE binary this is the ASLR slide that turns link-time symbol
/// values into runtime addresses; for ET_EXEC it is zero.
std::uintptr_t main_load_base() {
  std::uintptr_t base = 0;
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, std::size_t, void* data) {
        if (info->dlpi_name != nullptr && info->dlpi_name[0] == '\0') {
          *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
          return 1; // found the main executable — stop iterating
        }
        return 0;
      },
      &base);
  return base;
}

std::vector<std::uint8_t> read_whole_file(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error(std::string("cannot open ") + path);
  }
  const std::streamsize size = file.tellg();
  file.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    file.read(reinterpret_cast<char*>(bytes.data()), size);
  }
  return bytes;
}

const std::uint8_t* bounds(const std::vector<std::uint8_t>& bytes, std::uint64_t offset,
                           std::uint64_t len, const char* what) {
  // Overflow-proof bounds check (see object_file.cpp's at()).
  if (len > bytes.size() || offset > bytes.size() - len) {
    throw std::runtime_error(std::string("corrupt ELF: ") + what + " out of bounds");
  }
  return bytes.data() + offset;
}

} // namespace

process_symbols::process_symbols() {
  const auto bytes = read_whole_file("/proc/self/exe");

  const auto* ehdr =
      reinterpret_cast<const Elf64_Ehdr*>(bounds(bytes, 0, sizeof(Elf64_Ehdr), "ELF header"));
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
    throw std::runtime_error("/proc/self/exe: not ELF64 little-endian");
  }
  std::uintptr_t base = 0;
  if (ehdr->e_type == ET_DYN) {
    // PIE: every link-time symbol value needs the runtime load base.
    base = main_load_base();
    if (base == 0) {
      throw std::runtime_error(
          "/proc/self/exe is PIE but its load base cannot be determined (static PIE?)");
    }
  }

  const std::uint16_t shnum = ehdr->e_shnum;
  const std::vector<Elf64_Shdr> shdrs([&] {
    std::vector<Elf64_Shdr> out(shnum);
    for (std::uint16_t i = 0; i < shnum; ++i) {
      out[i] = *reinterpret_cast<const Elf64_Shdr*>(bounds(
          bytes, ehdr->e_shoff + i * ehdr->e_shentsize, sizeof(Elf64_Shdr), "section header"));
    }
    return out;
  }());

  for (const auto& sh : shdrs) {
    if (sh.sh_type != SHT_SYMTAB) {
      continue;
    }
    if (sh.sh_entsize != sizeof(Elf64_Sym)) {
      throw std::runtime_error("corrupt ELF: unexpected symbol entry size");
    }
    const auto& strhdr = shdrs[sh.sh_link];
    const char* strtab = reinterpret_cast<const char*>(
        bounds(bytes, strhdr.sh_offset, strhdr.sh_size, "symtab strtab"));
    const std::uint64_t count = sh.sh_size / sh.sh_entsize;
    for (std::uint64_t s = 1; s < count; ++s) {
      const auto& sym = *reinterpret_cast<const Elf64_Sym*>(
          bounds(bytes, sh.sh_offset + s * sh.sh_entsize, sizeof(Elf64_Sym), "symbol"));
      const auto type = ELF64_ST_TYPE(sym.st_info);
      if (type != STT_FUNC && type != STT_OBJECT) {
        continue;
      }
      if (sym.st_shndx == SHN_UNDEF || sym.st_name >= strhdr.sh_size) {
        continue;
      }
      const std::string name(strtab + sym.st_name);
      if (name.empty()) {
        continue;
      }
      if (type == STT_FUNC) {
        function_index_[name].push_back(functions_.size());
        functions_.push_back({name, static_cast<std::uintptr_t>(sym.st_value) + base,
                              static_cast<std::size_t>(sym.st_size)});
      } else {
        global_index_[name].push_back(globals_.size());
        globals_.push_back({name, static_cast<std::uintptr_t>(sym.st_value) + base,
                            static_cast<std::size_t>(sym.st_size)});
      }
    }
  }

  neko::log(log_level::info, "process symbols: %zu functions, %zu globals\n", functions_.size(),
            globals_.size());
}

std::vector<function_info> process_symbols::all_functions() const {
  return functions_;
}

std::optional<function_info> process_symbols::function_by_name(std::string_view name) const {
  const auto it = function_index_.find(std::string(name));
  return it != function_index_.end() && !it->second.empty()
             ? std::optional(functions_[it->second.front()])
             : std::nullopt;
}

std::size_t process_symbols::count_functions(std::string_view name) const {
  const auto it = function_index_.find(std::string(name));
  return it != function_index_.end() ? it->second.size() : 0;
}

std::size_t process_symbols::count_globals(std::string_view name) const {
  const auto it = global_index_.find(std::string(name));
  return it != global_index_.end() ? it->second.size() : 0;
}

std::optional<global_variable> process_symbols::global_by_name(std::string_view name) const {
  const auto it = global_index_.find(std::string(name));
  return it != global_index_.end() && !it->second.empty()
             ? std::optional(globals_[it->second.front()])
             : std::nullopt;
}

type_layout process_symbols::layout_of(type_id id) const {
  type_layout layout;
  layout.id = id;
  return layout; // layout queries are not meaningful yet.
}

void* process_symbols::map_global(std::string_view name) {
  const auto it = global_index_.find(std::string(name));
  return it != global_index_.end() && !it->second.empty()
             ? reinterpret_cast<void*>(globals_[it->second.front()].address)
             : nullptr;
}

void* process_symbols::resolve_external(std::string_view name) {
  return dlsym(RTLD_DEFAULT, std::string(name).c_str());
}

} // namespace neko::elf
