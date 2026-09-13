#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <neko/log.hpp>
#include <neko/platforms/elf.hpp>
#include <neko/runtime/depfile_planner.hpp>
#include <neko/session.hpp>

int a_value();
int b_value();

int main(int argc, char** argv) {
  if (argc != 8) {
    return 2;
  }

  const std::filesystem::path manifest = argv[1];
  const std::filesystem::path stop = argv[2];
  const std::filesystem::path working_directory = argv[7];

  auto backend = neko::elf::create_backend();
  backend.planner = std::make_shared<neko::depfile_planner>(std::vector<neko::depfile_entry>{
      {argv[3], argv[4], working_directory}, {argv[5], argv[6], working_directory}});

  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  neko::reload_session session{std::move(backend)};
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
