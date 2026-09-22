#pragma once

#include <neko/backend/session_driver.hpp>

namespace neko::backend {
// Backend-specific queries use this private seam, not an application driver escape hatch.
class session_access {
public:
  [[nodiscard]] static const session_driver* driver(const reload_session& session) noexcept {
    return session.impl_.get();
  }
};
} // namespace neko::backend
