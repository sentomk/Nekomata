// Managed end-to-end runner: embeds one reload group descriptor through the
// real `neko_groups` linker section, watches it, applies published
// generations at its frame safe points, and exercises the unwatch/resume
// contract. Run from a writable working directory: the descriptor's root
// hint resolves against it.

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

  const auto state_name = [](neko::group_state state) {
    switch (state) {
    case neko::group_state::idle:
      return "idle";
    case neko::group_state::preparing:
      return "preparing";
    case neko::group_state::ready:
      return "ready";
    case neko::group_state::failed:
      return "failed";
    }
    return "unknown";
  };

  const auto run_ticks = [&session, &state_name](int count) {
    for (int i = 0; i < count; ++i) {
      std::printf("tick=%d\n", managed_tick());
      const auto result = session.update();
      if (result.any_applied()) {
        std::printf("applied\n");
      }
      for (const auto& event : result.events) {
        if (event.status == neko::update_status::rejected) {
          std::printf("reload failed: %s\n", event.message.c_str());
        }
      }
      for (const auto& group : session.snapshot().managed_groups) {
        std::printf("state=%s\n", state_name(group.state));
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
