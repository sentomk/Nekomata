// Runs one generated 16-, 32-, or 64-TU managed reload group. The test
// script publishes complete generations whose root calls one brand-new
// symbol from every sibling object.

#include <neko/elf.hpp>
#include <neko/session.hpp>

#include "protocol/descriptor_section.hpp"
#include "protocol/group_descriptor.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifndef NEKO_SCALE_TU_COUNT
#error "NEKO_SCALE_TU_COUNT must name the generated group size"
#endif

extern "C" int scale_value_0();
extern "C" int scale_generation_marker();

namespace {

__attribute__((used, section("neko_groups"))) std::uint8_t k_embedded_groups[65536];

const bool k_section_initialized = [] {
  const std::string suffix = std::to_string(NEKO_SCALE_TU_COUNT);
  neko::detail::group_descriptor descriptor;
  descriptor.group_id = "//cross:scale-" + suffix;
  descriptor.publication_key = "cross-scale-" + suffix;
  descriptor.baseline_sequence = 0;
  descriptor.compatibility_id = "e2e-scale-compatibility-" + suffix;
  descriptor.abi_id = "elf-e2e-patch";
  descriptor.generation_root_hint = "neko-cross-scale";
  for (int index = 0; index < NEKO_SCALE_TU_COUNT; ++index) {
    descriptor.members.push_back("scale/unit-" + std::to_string(index));
  }
  const auto bytes = neko::detail::serialize_descriptor_section({descriptor});
  if (bytes.size() > sizeof(k_embedded_groups)) {
    return false;
  }
  std::memcpy(k_embedded_groups, bytes.data(), bytes.size());
  return true;
}();

} // namespace

int main() {
  if (!k_section_initialized) {
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  for (int tick = 0; tick < 6000; ++tick) {
    std::printf("marker=%d value=%d\n", scale_generation_marker(), scale_value_0());
    const auto result = session.update();
    for (const auto& event : result.events) {
      if (event.status == neko::update_status::applied) {
        std::printf("applied generation=%s redirected=%zu\n", event.generation_id.c_str(),
                    event.redirected_function_count);
      } else {
        std::printf("reload failed: %s\n", event.message.c_str());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}
