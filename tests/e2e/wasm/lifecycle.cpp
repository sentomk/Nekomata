#include "contract.hpp"

#include <backends/wasm/emscripten_loader.hpp>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <cstdlib>
#include <memory>
#include <utility>

namespace {
using namespace neko::wasm;

std::uint32_t completions = 0;
std::uint32_t stage = 0;
std::unique_ptr<candidate> pending;
active_module active;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });
EM_JS(void, release_download, (), { fetch("/lifecycle-frame", {method : "POST"}); });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

// Observe real loader completions even when the candidate has been destroyed.
class observed_loader final : public module_loader {
public:
  void open(std::string path, completion complete) override {
    loader_.open(std::move(path), [complete = std::move(complete)](module_load_result result) {
      require(result.image != nullptr, "lifetime fixture did not load");
      complete(std::move(result));
      ++completions;
    });
  }

private:
  emscripten_loader loader_;
};

bool frame(double, void*) {
  if (stage == 0) {
    release_download();
    stage = 1;
  }
  if (stage == 1 && completions == 1) {
    require(pending->status() == candidate_status::cancelled,
            "late completion revived cancelled candidate");
    require(!active.activate(*pending) && !active.current(), "cancelled code became active");
    pending.reset();
    {
      observed_loader temporary_loader;
      candidate abandoned(temporary_loader, "abandoned.wasm", flock_contract());
      require(abandoned.status() == candidate_status::loading,
              "abandoned fixture was not asynchronous");
    }
    stage = 2;
  }
  if (stage == 2 && completions == 2) {
    require(!active.current(), "destroyed candidate became active");
    observed_loader temporary_loader;
    pending = std::make_unique<candidate>(temporary_loader, "a.wasm", flock_contract());
    stage = 3;
  }
  if (stage == 3 && completions == 3) {
    require(pending->status() == candidate_status::ready, "valid candidate not ready");
    require(active.activate(*pending), "valid candidate failed after cancellation");
    auto snapshot = active.current();
    const auto identify = reinterpret_cast<identify_fn>(snapshot->entry("identify"));
    require(identify() == 1, "active identity");
    {
      observed_loader temporary_loader;
      candidate cached(temporary_loader, "a.wasm", flock_contract());
      require(cached.status() == candidate_status::ready && completions == 4,
              "cached completion was not synchronous");
      cached.cancel();
      require(!active.activate(cached), "cancelled ready candidate activated");
    }
    require(active.current() == snapshot && identify() == 1,
            "discarding cached candidate damaged active code");
    pending.reset();
    active = active_module{};
    snapshot.reset();
    require(identify() == 1, "committed code did not survive its C++ owners");
    report(1, "late completion after cancellation/destruction, cached completion and resident code "
              "passed");
    return false;
  }
  return true;
}

} // namespace

int main() {
  {
    observed_loader temporary_loader;
    pending = std::make_unique<candidate>(temporary_loader, "late.wasm", flock_contract());
    require(pending->status() == candidate_status::loading, "late fixture was not asynchronous");
    pending->cancel();
    pending->cancel();
  }
  emscripten_request_animation_frame_loop(frame, nullptr);
  emscripten_exit_with_live_runtime();
}
