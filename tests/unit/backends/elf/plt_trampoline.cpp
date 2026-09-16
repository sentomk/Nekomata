// PLT trampoline ABI tests for the ELF loader.
//
// Calls to undefined (external) symbols cannot reach libc with a rel32
// call, so the loader routes them through an in-arena trampoline. That
// trampoline once read `movabs rax, imm64; jmp rax` — but at a call site
// RAX is live: AL carries the SSE-argument count into variadic callees
// (SysV). With a target whose low address byte was 0, printf skipped its
// SSE save area and every %.1f silently printed 0.0 — caught by the
// playground demo, whose HUD doubles all read zero while %d/%s kept
// working. These tests pin the call-site-safe form
// `movabs r11, imm64; jmp r11`, which leaves the argument registers
// untouched.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "code_pages.hpp"
#include "loader.hpp"
#include "process_symbols.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::filesystem::path fixture;

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

volatile int plt_sink; // written by neko_plt_probe_entry to keep it patch-sized

} // namespace

// The fixture object refers to these two functions by name; the test binary
// provides the definitions so the loader finds both a live redirect
// candidate and a deterministic external target in /proc/self/exe.
extern "C" void neko_plt_probe_target(const char*, ...) {
  // Resolution target only — the test never calls it.
}

extern "C" void neko_plt_probe_entry() {
  // Exists so the loader's hint search matches a live function by name.
  // Big enough to stay patchable (the loader refuses entries under 8 bytes
  // when collecting replacements): a few volatile stores do it.
  plt_sink = 1;
  plt_sink = 2;
  plt_sink = 3;
}

TEST_CASE("external calls are routed through a call-site-safe trampoline") {
  neko::elf::process_symbols symbols;
  neko::elf::code_pages pages;
  neko::elf::loader ldr(symbols, symbols, pages);

  const auto bytes = read_file(fixture);
  REQUIRE(!bytes.empty());
  const auto image = ldr.load(bytes.data(), bytes.size());
  REQUIRE(image.code != nullptr);

  // The arena holds [text][rodata][trampolines]: locate the 13-byte window
  // whose imm64 is our probe target's address.
  const auto* const begin = static_cast<const std::uint8_t*>(image.code);
  const auto* const end = begin + image.code_size;
  const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(&neko_plt_probe_target);

  bool found_r11 = false;
  bool found_rax = false;
  for (const std::uint8_t* p = begin; p + 13 <= end; ++p) {
    if (std::memcmp(p + 2, &target, sizeof(target)) != 0) {
      continue;
    }
    found_r11 = found_r11 || (p[0] == 0x49 && p[1] == 0xBB &&                   // movabs r11, imm64
                              p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3); // jmp r11
    found_rax = found_rax || (p[0] == 0x48 && p[1] == 0xB8 &&                   // movabs rax, imm64
                              p[10] == 0xFF && p[11] == 0xE0);                  // jmp rax
  }
  CHECK(found_r11);  // movabs r11, imm64 ; jmp r11
  CHECK(!found_rax); // the movabs rax form corrupts AL for variadic callees
}

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  fixture = argv[1];
  doctest::Context context;
  return context.run();
}
