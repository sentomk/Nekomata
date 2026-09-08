// hello_reload — acceptance demo runner.
//
// Integration is the three marked lines: create a session with the ELF
// backend, watch a path, tick. Everything else is demo scaffolding.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

void tick(); // defined in hot.cpp — swapped live by nekomata

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "hot.new.o";
  const char* stop_file = argc > 2 ? argv[2] : nullptr;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  neko::reload_session session{neko::elf::create_backend()}; // 1. agent
  session.watch(watched);                                    // 2. watch a path
  neko::log(neko::log_level::info, "watching '%s' (pid %d) — drop a fresh hot.o to reload\n",
            watched, static_cast<int>(getpid()));

  for (int i = 0; i < 2000; ++i) { // ~6.5 min at 200 ms; the demo exits sooner
    if (stop_file && std::filesystem::exists(stop_file)) {
      std::printf("[harness] stopped\n");
      return 0;
    }
    tick(); //    hot code
    try {
      session.update(); // 3. per-iteration tick
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return stop_file ? 1 : 0; // A harness run must acknowledge its stop request.
}
