// libFuzzer target: the ELF relocatable-object parser.
//
// Loud rejections (std::runtime_error) are the contract — the catch turns
// them into PASS. Crashes, hangs and sanitizer reports are the bugs.
// Seeds live in tests/fuzz/corpus; the CI burst replays them all, then
// explores for a few minutes.

#include <cstddef>
#include <cstdint>
#include <exception>

#include "object_file.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    const auto parsed = neko::elf::parse_object(data, size);
    (void)parsed;
  } catch (const std::exception&) {
    // rejected loudly — fine
  }
  return 0;
}
