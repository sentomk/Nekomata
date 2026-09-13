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
  if (argc != 7) {
    return 2;
  }

  const std::filesystem::path a_offer = argv[1];
  const std::filesystem::path a_source = argv[2];
  const std::filesystem::path b_offer = argv[3];
  const std::filesystem::path b_source = argv[4];
  const std::filesystem::path trigger = argv[5];
  const std::filesystem::path stop = argv[6];

  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{neko::elf::create_backend()};
  session.watch(a_offer, a_source);
  session.watch(b_offer, b_source);

  for (int i = 0; i < 3000; ++i) {
    std::printf("[pair %d %d]\n", a_value(), b_value());

    std::error_code ec;
    if (std::filesystem::remove(trigger, ec)) {
      try {
        static_cast<void>(session.update());
      } catch (const std::exception& exception) {
        neko::log(neko::log_level::error, "reload failed, keeping old code: %s\n",
                  exception.what());
      }
    }
    if (std::filesystem::exists(stop, ec)) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 1;
}
