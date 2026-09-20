#pragma once

#include <cstdint>

namespace neko::wasm {

// Private, cooperative module contract. All participants use the same toolchain
// and ABI; descriptor pointers are trusted, not a parser for hostile memory.
inline constexpr std::uint32_t module_interface_version = 1;
inline constexpr const char* descriptor_export = "neko_wasm_descriptor";

using module_function = void (*)();

struct module_header {
  std::uint32_t version;
  std::uint32_t size;
};

struct module_entry {
  const char* name;
  module_function address;
};

struct module_descriptor {
  module_header header;
  const char* abi_id;
  std::uint32_t entry_count;
  const module_entry* entries;
};

using descriptor_function = const module_header* (*)();

} // namespace neko::wasm
