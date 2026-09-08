// runner.cpp — atomicity host. Calls all three hot functions each tick so
// partial states are directly observable in the output.

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <unistd.h>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

void tick();
void tock();
void naked_trouble();

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "atomic.new.o";
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{neko::elf::create_backend()};
  session.watch(watched);

  for (int i = 0; i < 3000; ++i) {
    tick();
    tock();
    naked_trouble();
    try {
      session.update();
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  return 0;
}
