// runner.cpp — host for the rejection-cases harness.
//
// The test script drops deliberately broken object files at the watched
// path; every case must end with: a specific rejection message, the
// process still alive, and the OLD code still running. Startup failures
// (PIE host) are reported as "startup rejected:" and exit non-zero.

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

void tick();

// Defined in the host only — the cross-TU case has the fresh code call it.
void host_only() {
  std::printf("[host] host_only called\n");
}

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "bad.new.o";
  const char* stop_file = argc > 2 ? argv[2] : nullptr;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  std::unique_ptr<neko::reload_session> session;
  try {
    session = std::make_unique<neko::reload_session>(neko::elf::create_backend());
  } catch (const std::exception& e) {
    std::printf("startup rejected: %s\n", e.what());
    return 1;
  }
  session->watch(watched);

  for (int i = 0; i < 600; ++i) { // ~60 s ceiling; the script requests an earlier stop
    if (stop_file && std::filesystem::exists(stop_file)) {
      std::printf("[harness] stopped\n");
      return 0;
    }
    tick();
    try {
      session->update();
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::printf("[harness] survived\n");
  return stop_file ? 1 : 0;
}
