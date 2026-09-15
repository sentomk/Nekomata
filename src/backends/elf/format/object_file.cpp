#include "object_file.hpp"

#include <elf.h>

#include <stdexcept>
#include <string>

namespace neko::elf {
namespace {

const std::uint8_t* at(const std::uint8_t* base, std::size_t size, std::uint64_t offset,
                       std::uint64_t bytes, const char* what) {
  // Overflow-proof bounds check: the additive form (offset + bytes > size)
  // wraps around uint64 on crafted headers and lets huge offsets through
  // (found by libFuzzer). The subtractive form cannot wrap.
  if (bytes > size || offset > size - bytes) {
    throw std::runtime_error(std::string("truncated object file: ") + what + " out of bounds");
  }
  return base + offset;
}

section_class classify(std::uint64_t flags) {
  if (!(flags & SHF_ALLOC)) {
    return section_class::other;
  }
  if (flags & SHF_EXECINSTR) {
    return section_class::text;
  }
  if (flags & SHF_WRITE) {
    return section_class::data;
  }
  // SHT_NOBITS without SHF_WRITE does not exist in practice; rodata it is.
  return section_class::rodata;
}

} // namespace

object_file parse_object(const std::uint8_t* data, std::size_t size) {
  auto require = [](bool ok, const char* what) {
    if (!ok) {
      throw std::runtime_error(std::string("not a supported object file: ") + what);
    }
  };

  const auto* ehdr =
      reinterpret_cast<const Elf64_Ehdr*>(at(data, size, 0, sizeof(Elf64_Ehdr), "ELF header"));
  require(ehdr->e_ident[EI_MAG0] == ELFMAG0 && ehdr->e_ident[EI_MAG1] == ELFMAG1 &&
              ehdr->e_ident[EI_MAG2] == ELFMAG2 && ehdr->e_ident[EI_MAG3] == ELFMAG3,
          "bad magic");
  require(ehdr->e_ident[EI_CLASS] == ELFCLASS64, "not ELF64");
  require(ehdr->e_ident[EI_DATA] == ELFDATA2LSB, "not little-endian");
  require(ehdr->e_type == ET_REL, "not a relocatable object (ET_REL)");
  require(ehdr->e_machine == EM_X86_64, "not x86-64");
  require(ehdr->e_shentsize == sizeof(Elf64_Shdr), "unexpected section header size");

  const std::uint16_t shnum = ehdr->e_shnum;
  const std::uint16_t shstrndx = ehdr->e_shstrndx;
  require(shnum > 0, "no sections");
  require(shstrndx < shnum, "bad section name string table index");

  std::vector<Elf64_Shdr> shdrs(shnum);
  for (std::uint16_t i = 0; i < shnum; ++i) {
    shdrs[i] = *reinterpret_cast<const Elf64_Shdr*>(
        at(data, size, ehdr->e_shoff + static_cast<std::uint64_t>(i) * ehdr->e_shentsize,
           sizeof(Elf64_Shdr), "section header"));
  }

  const auto& shstr = shdrs[shstrndx];
  const char* shstrtab =
      reinterpret_cast<const char*>(at(data, size, shstr.sh_offset, shstr.sh_size, "shstrtab"));

  auto section_name = [&](std::uint32_t offset) -> std::string {
    if (offset >= shstr.sh_size) {
      throw std::runtime_error("section name offset out of bounds");
    }
    return std::string(shstrtab + offset);
  };

  object_file obj;
  obj.sections.reserve(shnum);
  for (std::uint16_t i = 0; i < shnum; ++i) {
    const auto& sh = shdrs[i];
    section sec;
    sec.index = i;
    sec.name = section_name(sh.sh_name);
    sec.cls = classify(sh.sh_flags);
    sec.size = sh.sh_size;
    if (sh.sh_addralign != 0) {
      require((sh.sh_addralign & (sh.sh_addralign - 1)) == 0,
              "section alignment is not a power of two");
      sec.align = sh.sh_addralign;
    }
    if (sh.sh_type == SHT_PROGBITS && sec.cls != section_class::other) {
      const std::uint8_t* start = at(data, size, sh.sh_offset, sh.sh_size, sec.name.c_str());
      sec.bytes.assign(start, start + sh.sh_size);
    }
    obj.sections.push_back(std::move(sec));
  }

  for (std::uint16_t i = 0; i < shnum; ++i) {
    const auto& sh = shdrs[i];
    if (sh.sh_type == SHT_SYMTAB) {
      require(sh.sh_link < shnum, "symtab strtab index out of bounds");
      // Entry size is checked BEFORE dividing: a crafted header with
      // sh_entsize == 0 is a division-by-zero crash otherwise (found by
      // libFuzzer on its first run).
      require(sh.sh_entsize == sizeof(Elf64_Sym), "unexpected symbol size");
      const auto& strhdr = shdrs[sh.sh_link];
      const char* strtab = reinterpret_cast<const char*>(
          at(data, size, strhdr.sh_offset, strhdr.sh_size, "symtab strtab"));
      const std::uint64_t count = sh.sh_size / sh.sh_entsize;
      // Keep symbol index 0 (the null symbol): relocations reference
      // symbols by their table index, so the vector must line up with it.
      for (std::uint64_t s = 0; s < count; ++s) {
        const auto& sym = *reinterpret_cast<const Elf64_Sym*>(
            at(data, size, sh.sh_offset + s * sh.sh_entsize, sizeof(Elf64_Sym), "symbol"));
        std::string name =
            sym.st_name < strhdr.sh_size ? std::string(strtab + sym.st_name) : std::string();
        symbol out;
        out.name = std::move(name);
        out.section_index = sym.st_shndx;
        out.type = ELF64_ST_TYPE(sym.st_info);
        out.bind = ELF64_ST_BIND(sym.st_info);
        out.value = sym.st_value;
        out.size = sym.st_size;
        obj.symbols.push_back(std::move(out));
      }
    } else if (sh.sh_type == SHT_RELA) {
      require(sh.sh_info < shnum, "relocation target section out of bounds");
      require(sh.sh_entsize == sizeof(Elf64_Rela), "unexpected relocation size");
      const std::uint64_t count = sh.sh_size / sh.sh_entsize;
      for (std::uint64_t r = 0; r < count; ++r) {
        const auto& rela = *reinterpret_cast<const Elf64_Rela*>(
            at(data, size, sh.sh_offset + r * sh.sh_entsize, sizeof(Elf64_Rela), "relocation"));
        relocation out;
        out.target_section = static_cast<std::uint16_t>(sh.sh_info);
        out.offset = rela.r_offset;
        out.symbol_index = static_cast<std::uint32_t>(ELF64_R_SYM(rela.r_info));
        out.type = static_cast<std::uint32_t>(ELF64_R_TYPE(rela.r_info));
        out.addend = rela.r_addend;
        obj.relocations.push_back(out);
      }
    }
  }

  return obj;
}

} // namespace neko::elf
