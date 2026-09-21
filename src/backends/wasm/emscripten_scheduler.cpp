#include "emscripten_scheduler.hpp"

#include <emscripten/eventloop.h>

#include <utility>

namespace neko::wasm {
namespace {

class interval_subscription final : public poll_subscription {
public:
  explicit interval_subscription(std::function<void()> callback) : callback_(std::move(callback)) {
    id_ = emscripten_set_interval(
        [](void* context) {
          // The callback can destroy its own subscription. Keep the function
          // alive and do not access the context after invoking it.
          auto invoke = static_cast<interval_subscription*>(context)->callback_;
          invoke();
        },
        100.0, this);
  }
  ~interval_subscription() override { emscripten_clear_interval(id_); }

private:
  std::function<void()> callback_;
  int id_;
};

} // namespace

std::unique_ptr<poll_subscription>
emscripten_poll_scheduler::repeat(std::function<void()> callback) {
  return std::make_unique<interval_subscription>(std::move(callback));
}

} // namespace neko::wasm
