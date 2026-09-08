// runner.cpp — soak host. Runs a bounded tick loop, counts applied
// reloads, and censuses its own memory map for our arenas (an r-xp
// region immediately followed by its PROT_NONE guard page). The driver
// script drops randomized variants; this binary proves continuity and
// accounting across every generation.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

void tick();

namespace {

// Our arena signature in /proc/self/maps: an executable anonymous region
// directly followed by a PROT_NONE page (the guard armed by code_pages).
std::size_t count_arenas_from_maps() {
  std::ifstream maps("/proc/self/maps");
  std::size_t arenas = 0;
  bool prev_exec = false;
  std::uintptr_t prev_end = 0;
  std::string line;
  while (std::getline(maps, line)) {
    std::uintptr_t start = 0, end = 0;
    char perms[5] = {};
    if (std::sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3) {
      continue;
    }
    const bool guard = perms[0] == '-' && perms[1] == '-';
    if (guard && prev_exec && start == prev_end) {
      ++arenas;
    }
    prev_exec = perms[0] == 'r' && perms[2] == 'x';
    prev_end = end;
  }
  return arenas;
}

} // namespace

namespace {
volatile sig_atomic_t g_stop = 0;
void on_sigterm(int) {
  g_stop = 1;
}
} // namespace

int main(int argc, char** argv) {
  std::signal(SIGTERM, on_sigterm);
  const char* watched = argc > 1 ? argv[1] : "soak.new.o";
  const int max_ticks = argc > 2 ? std::atoi(argv[2]) : 3000;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const std::size_t arenas_before = count_arenas_from_maps();

  neko::reload_session session{neko::elf::create_backend()};
  session.watch(watched);

  std::size_t applied = 0;
  for (int i = 0; i < max_ticks && g_stop == 0; ++i) {
    tick();
    try {
      if (session.update()) {
        ++applied;
      }
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }

  const std::size_t arenas_after = count_arenas_from_maps();
  std::printf("summary applied=%zu arenas_before=%zu arenas_after=%zu\n", applied, arenas_before,
              arenas_after);
  return 0;
}
