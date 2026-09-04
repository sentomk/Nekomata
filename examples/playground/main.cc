// main.cc —— playground 宿主。接入 nekomata 就是标了序号的三行。
// 这个文件不需要热改；要改的都在 demo.cc 里。

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <unistd.h>

#include <neko/elf.hpp>
#include <neko/log.hpp>
#include <neko/session.hpp>

void step_world();
void render_world();

int main(int argc, char** argv) {
    const char* watched = argc > 1 ? argv[1] : "demo.new.o";
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    neko::reload_session session{neko::elf::create_backend()}; // 1. agent
    session.watch(watched);                                    // 2. watch

    std::printf("\033[2J\033[H"); // 清屏一次
    neko::log(neko::log_level::info,
              "playground running (pid %d) — edit demo.cc, then run "
              "./reload.sh\n",
              static_cast<int>(getpid()));

    for (int i = 0; i < 100000; ++i) {
        step_world();
        render_world();
        try {
            session.update(); // 3. 每帧 tick：发现新 .o 就热替换
        } catch (const std::exception& e) {
            neko::log(neko::log_level::error, "reload rejected, old code keeps running: %s\n",
                      e.what());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return 0;
}
