// Consumer of the CMake adapter fixture group: the documented managed
// integration — construct, watch(), update() at the frame safe point.

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

extern "C" int adapter_tick();

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const int max_ticks = std::atoi(argv[1]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  for (int i = 0; i < max_ticks; ++i) {
    std::printf("tick=%d\n", adapter_tick());
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
  session.unwatch();
  return 0;
}
