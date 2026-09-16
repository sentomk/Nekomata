// Consumer of the heterogeneous group: two units, one atomic commit.

#include <neko/elf.hpp>
#include <neko/log.hpp>
#include <neko/session.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

extern "C" int mixed_physics();
extern "C" int mixed_extra();

int main(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const int max_ticks = std::atoi(argv[1]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()};
  session.watch();

  for (int i = 0; i < max_ticks; ++i) {
    std::printf("phys=%d extra=%d\n", mixed_physics(), mixed_extra());
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
