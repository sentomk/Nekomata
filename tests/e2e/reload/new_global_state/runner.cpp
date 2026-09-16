#include <neko/elf.hpp>
#include <neko/session.hpp>

#include "protocol/descriptor_section.hpp"
#include "protocol/group_descriptor.hpp"
#include "state_api.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

__attribute__((used, section("neko_groups"))) std::uint8_t k_embedded_groups[2048];

const bool k_section_initialized = [] {
  neko::detail::group_descriptor descriptor;
  descriptor.group_id = "//state:lifecycle";
  descriptor.members = {"state/a", "state/b"};
  descriptor.publication_key = "new-global-state-e2e";
  descriptor.baseline_sequence = 0;
  descriptor.compatibility_id = "new-global-state-compatibility";
  descriptor.abi_id = "elf-e2e-patch";
  descriptor.generation_root_hint = "neko-new-global-state";
  const auto bytes = neko::detail::serialize_descriptor_section({descriptor});
  std::memcpy(k_embedded_groups, bytes.data(), bytes.size());
  return true;
}();

} // namespace

int main() {
  static_cast<void>(k_section_initialized);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  for (int tick = 0; tick < 1200; ++tick) {
    const int marker = state_marker();
    const int a = state_a_value();
    const int b = state_b_value();
    const int shared = state_shared_value();
    const int retry = state_retry_value();
    std::printf("marker=%d a=%d b=%d shared=%d retry=%d\n", marker, a, b, shared, retry);
    const auto result = session.update();
    for (const auto& event : result.events) {
      if (event.status == neko::update_status::applied) {
        std::printf("applied generation=%s\n", event.generation_id.c_str());
      } else if (event.status == neko::update_status::rejected) {
        std::printf("reload failed: %s\n", event.message.c_str());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return 0;
}
