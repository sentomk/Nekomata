#pragma once

#include "poll_scheduler.hpp"

namespace neko::wasm {

// A browser event-loop poll every 100 ms, independent of frame commits.
class emscripten_poll_scheduler final : public poll_scheduler {
public:
  std::unique_ptr<poll_subscription> repeat(std::function<void()> callback) override;
};

} // namespace neko::wasm
