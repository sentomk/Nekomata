// Cross-TU multi-fixup end-to-end runner: one managed group of four members.
// The published version makes one member call three brand-new symbols, each
// defined by a different sibling object — so one image carries three pending
// cross-object call fixups into link_generation.

#include <neko/elf.hpp>
#include <neko/log.hpp>
#include <neko/session.hpp>

#include "protocol/descriptor_section.hpp"
#include "protocol/group_descriptor.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

extern "C" int a_value();

namespace {

__attribute__((used, section("neko_groups"))) std::uint8_t k_embedded_groups[2048];

const bool k_section_initialized = [] {
  neko::detail::group_descriptor descriptor;
  descriptor.group_id = "//cross:multi";
  descriptor.members = {"cross/a", "cross/b", "cross/c", "cross/d"};
  descriptor.publication_key = "cross-multi-e2e";
  descriptor.baseline_sequence = 0;
  descriptor.compatibility_id = "e2e-compatibility";
  descriptor.abi_id = "elf-e2e-patch";
  descriptor.generation_root_hint = "neko-cross-multi";
  const auto bytes = neko::detail::serialize_descriptor_section({descriptor});
  std::memcpy(k_embedded_groups, bytes.data(), bytes.size());
  return true;
}();

} // namespace

int main(int argc, char** argv) {
  static_cast<void>(k_section_initialized);
  if (argc != 2) {
    return 2;
  }
  const int max_ticks = std::atoi(argv[1]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  for (int i = 0; i < max_ticks; ++i) {
    std::printf("tick=%d\n", a_value());
    const auto result = session.update();
    if (result.any_applied()) {
      std::printf("applied\n");
    }
    for (const auto& event : result.events) {
      if (event.status == neko::update_status::rejected) {
        std::printf("reload failed: %s\n", event.message.c_str());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}
