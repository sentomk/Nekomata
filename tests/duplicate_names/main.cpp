// main.cpp — duplicate-names host: prints both TUs' values every tick.
// After a rejected offer of a.cpp v2 (value -> 100), BOTH must be unchanged:
// a==1 because nothing was patched, b==2 because it was never a_candidate.

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

int a_value();
int b_value();

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "dup.new.o";
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{neko::elf::create_backend()};
  session.watch(watched);

  for (int i = 0; i < 3000; ++i) {
    std::printf("[a=%d b=%d]\n", a_value(), b_value());
    try {
      session.update();
    } catch (const std::exception& e) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  return 0;
}
