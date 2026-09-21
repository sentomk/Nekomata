#pragma once

#include <functional>
#include <memory>

namespace neko::wasm {

// Destroying the subscription stops future scheduling. A queued callback may
// still arrive; its owner must guard against a disabled or destroyed observer.
class poll_subscription {
public:
  virtual ~poll_subscription() = default;
};

class poll_scheduler {
public:
  virtual ~poll_scheduler() = default;
  // Repeated callbacks run on the caller's event loop, never inside repeat().
  // Return an owning subscription, or throw if scheduling cannot start.
  virtual std::unique_ptr<poll_subscription> repeat(std::function<void()> callback) = 0;
};

} // namespace neko::wasm
