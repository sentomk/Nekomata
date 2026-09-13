#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <thread>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/session.hpp>

int a_value();
int b_value();

int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }

  const std::filesystem::path manifest = argv[1];
  const std::filesystem::path stop = argv[2];

  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{neko::elf::create_backend()};
  session.watch(neko::generation_watch{manifest});

  for (int i = 0; i < 3000; ++i) {
    std::printf("[pair %d %d]\n", a_value(), b_value());

    try {
      static_cast<void>(session.update());
    } catch (const std::exception& exception) {
      neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n", exception.what());
    }
    std::error_code ec;
    if (std::filesystem::exists(stop, ec)) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 1;
}
