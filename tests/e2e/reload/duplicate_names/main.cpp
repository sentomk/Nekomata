// main.cpp — duplicate-names host: prints both TUs' values every tick.
// With a source identity and manifest, a.cpp v2 must change a to 100 while b
// stays 2. Without those inputs, the ambiguous offer must be rejected.

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

#include <neko/elf.hpp>
#include <neko/log.hpp>
#include <neko/session.hpp>

int a_value();
int b_value();

int main(int argc, char** argv) {
  const char* watched = argc > 1 ? argv[1] : "dup.new.o";
  const char* source = argc > 2 ? argv[2] : nullptr;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{neko::elf::create_backend()};
  if (source != nullptr) {
    session.watch(watched, source);
  } else {
    session.watch(std::filesystem::path{watched});
  }

  for (int i = 0; i < 3000; ++i) {
    std::printf("[a=%d b=%d]\n", a_value(), b_value());
    const auto result = session.update();
    for (const auto& event : result.events) {
      if (event.status == neko::update_status::rejected) {
        neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n",
                  event.message.c_str());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  return 0;
}
