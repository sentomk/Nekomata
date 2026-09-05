// Playground host. The three numbered lines integrate Nekomata.
// Keep this file unchanged while running; edit demo.cpp to reload behavior.

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <unistd.h>

#include <neko/backends/elf.hpp>
#include <neko/core/log.hpp>
#include <neko/runtime/session.hpp>

void step_world();
void render_world();

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "demo.new.o";
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()}; // 1. agent
  session.watch(watched);                                    // 2. watch

  std::printf("\033[2J\033[H"); // Clear the screen once.
  neko::log(neko::log_level::info,
            "playground running (pid %d) — edit demo.cpp, then run "
            "./reload.sh\n",
            static_cast<int>(getpid()));

  for (int i = 0; i < 100000; ++i) {
    step_world();
    render_world();
    try {
      session.update(); // 3. Check for a fresh object file once per frame.
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload rejected, old code keeps running: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return 0;
}
