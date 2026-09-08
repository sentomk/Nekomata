#include "process_symbols.hpp"

#include <elf.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <neko/log.hpp>

namespace neko::elf {
namespace {

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
  if (offset + len > bytes.size()) {
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
  if (ehdr->e_type == ET_DYN) {
    throw std::runtime_error("/proc/self/exe is position-independent (PIE), which is not "
                             "supported yet — relink with -no-pie");
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
        function_index_[name] = functions_.size();
        functions_.push_back({name, static_cast<std::uintptr_t>(sym.st_value),
                              static_cast<std::size_t>(sym.st_size)});
      } else {
        global_index_[name] = globals_.size();
        globals_.push_back({name, static_cast<std::uintptr_t>(sym.st_value),
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
  return it != function_index_.end() ? std::optional(functions_[it->second]) : std::nullopt;
}

std::optional<global_variable> process_symbols::global_by_name(std::string_view name) const {
  const auto it = global_index_.find(std::string(name));
  return it != global_index_.end() ? std::optional(globals_[it->second]) : std::nullopt;
}

type_layout process_symbols::layout_of(type_id id) const {
  type_layout layout;
  layout.id = id;
  return layout; // layout queries are not meaningful yet.
}

void* process_symbols::map_global(std::string_view name) {
  const auto it = global_index_.find(std::string(name));
  return it != global_index_.end() ? reinterpret_cast<void*>(globals_[it->second].address)
                                   : nullptr;
}

void* process_symbols::resolve_external(std::string_view name) {
  return dlsym(RTLD_DEFAULT, std::string(name).c_str());
}

} // namespace neko::elf
