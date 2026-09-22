#pragma once

#include <neko/session.hpp>

namespace neko::backend {

// Platform lifecycle interface, owned exclusively by one public reload_session.
// Factories hide this interface from ordinary callers. Calls are serialized by
// the application; update() runs only while reloadable code is quiescent.
class session_driver {
public:
  virtual ~session_driver() = default;
  virtual void watch() = 0;
  virtual void watch(std::string_view group_id) = 0;
  virtual void unwatch() = 0;
  virtual void unwatch(std::string_view group_id) = 0;
  [[nodiscard]] virtual update_result update() = 0;
  [[nodiscard]] virtual session_snapshot snapshot() const = 0;

  // Browser and other managed-only implementations reject legacy object watches.
  virtual void watch(std::filesystem::path object_path);
  virtual void watch(std::filesystem::path object_path, const std::filesystem::path& source_path);
};

} // namespace neko::backend
