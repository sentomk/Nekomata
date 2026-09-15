// Playground host. The three numbered lines integrate Nekomata.
// Keep this file unchanged while running; edit demo.cpp to reload behavior.

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <unistd.h>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

void step_world();
void render_world();

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "demo.new.o";
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()}; // 1. agent
  session.watch(std::filesystem::path{watched});             // 2. watch

  std::printf("\033[2J\033[H"); // Clear the screen once.
  neko::log(neko::log_level::info,
            "playground running (pid %d) — edit demo.cpp, then run "
            "./reload.sh\n",
            static_cast<int>(getpid()));

  for (int i = 0; i < 100000; ++i) {
    step_world();
    render_world();
    const auto result = session.update(); // 3. per-iteration tick
    for (const auto& event : result.events) {
      if (event.status == neko::update_status::rejected) {
        neko::log(neko::log_level::error, "reload rejected, old code keeps running: %s\n",
                  event.message.c_str());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return 0;
}
