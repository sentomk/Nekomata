// End-to-end discovery through a real linker section (Linux/ELF only): the
// linker synthesizes __start_neko_groups/__stop_neko_groups for a named
// input section, exactly as it will for build-generated descriptor TUs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runtime/descriptor_discovery.hpp"
#include "runtime/group_descriptor.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

neko::detail::group_descriptor descriptor_a() {
  neko::detail::group_descriptor value;
  value.group_id = "//gameplay:hot";
  value.members = {"gameplay/ball", "gameplay/gravity"};
  value.publication_key = "gameplay-6f10e51d";
  value.baseline_sequence = 42;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  return value;
}

neko::detail::group_descriptor descriptor_b() {
  auto value = descriptor_a();
  value.group_id = "//editor:inspector";
  value.members = {"editor/inspector"};
  value.publication_key = "editor-9c1f7a30";
  value.baseline_sequence = 7;
  return value;
}

// Writable so the framed records — A, an identical duplicate of A, and B —
// can be filled in before main; the zero tail doubles as the alignment
// padding the parser must tolerate.
__attribute__((used, section("neko_groups"))) std::uint8_t k_embedded_groups[1024];

const bool k_section_initialized = [] {
  std::size_t offset = 0;
  for (const auto& descriptor : {descriptor_a(), descriptor_a(), descriptor_b()}) {
    const auto payload = neko::detail::serialize_group_descriptor(descriptor);
    const auto length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
      k_embedded_groups[offset++] = static_cast<std::uint8_t>(length >> (i * 8));
    }
    std::memcpy(k_embedded_groups + offset, payload.data(), payload.size());
    offset += payload.size();
  }
  return true;
}();

} // namespace

TEST_CASE("embedded descriptors are discovered through the linker section") {
  REQUIRE(k_section_initialized);
  const auto discovered = neko::detail::discover_embedded_descriptors();
  REQUIRE(discovered.size() == 2); // the identical duplicate collapses
  CHECK(discovered[0] == descriptor_a());
  CHECK(discovered[1] == descriptor_b());
}
