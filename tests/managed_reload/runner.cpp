// Managed end-to-end runner: embeds one reload group descriptor through the
// real `neko_groups` linker section, watches it, applies published
// generations at its frame safe points, and exercises the unwatch/resume
// contract. Run from a writable working directory: the descriptor's root
// hint resolves against it.

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

#include "runtime/descriptor_section.hpp"
#include "runtime/group_descriptor.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <thread>

extern "C" int managed_tick();

namespace {

__attribute__((used, section("neko_groups"))) std::uint8_t k_embedded_groups[2048];

const bool k_section_initialized = [] {
  neko::detail::group_descriptor descriptor;
  descriptor.group_id = "//managed:hot";
  descriptor.members = {"managed/hot"};
  descriptor.publication_key = "managed-watch-e2e";
  descriptor.baseline_sequence = 0;
  descriptor.compatibility_id = "e2e-compatibility";
  descriptor.abi_id = "elf-e2e-patch";
  descriptor.generation_root_hint = "neko-managed-watch";
  const auto bytes = neko::detail::serialize_descriptor_section({descriptor});
  std::memcpy(k_embedded_groups, bytes.data(), bytes.size());
  return true;
}();

} // namespace

int main(int argc, char** argv) {
  static_cast<void>(k_section_initialized);
  if (argc != 4) {
    return 2;
  }
  const int watched_ticks = std::atoi(argv[1]);
  const int unwatched_ticks = std::atoi(argv[2]);
  const int resumed_ticks = std::atoi(argv[3]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  const auto run_ticks = [&session](int count) {
    for (int i = 0; i < count; ++i) {
      std::printf("tick=%d\n", managed_tick());
      try {
        if (session.update()) {
          std::printf("applied\n");
        }
      } catch (const std::exception& exception) {
        std::printf("reload failed: %s\n", exception.what());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  };

  run_ticks(watched_ticks);
  std::printf("phase=unwatched\n");
  session.unwatch();
  run_ticks(unwatched_ticks);
  std::printf("phase=resumed\n");
  session.watch();
  run_ticks(resumed_ticks);
  return 0;
}
