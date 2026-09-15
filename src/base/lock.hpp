#pragma once

#include "c/neko_lock.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace neko::detail {

/// Exclusive, blocking cross-process file lock for the duration of one
/// scope. Thin exception-safety wrapper over the private C lock layer; what
/// a lock protects is the caller's policy.
class file_lock {
public:
  explicit file_lock(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string encoded = path.u8string();
    const char* raw = reinterpret_cast<const char*>(encoded.c_str());
#else
    const std::string encoded = path.generic_string();
    const char* raw = encoded.c_str();
#endif
    const int status = neko_lock_acquire(&lock_, raw);
    if (status == neko_lock_open_failed) {
      throw std::runtime_error("cannot open lock file '" + path.generic_string() + "'");
    }
    if (status != neko_lock_ok) {
      throw std::runtime_error("cannot acquire lock '" + path.generic_string() + "'");
    }
  }

  ~file_lock() { neko_lock_release(&lock_); }

  file_lock(const file_lock&) = delete;
  file_lock& operator=(const file_lock&) = delete;

private:
  neko_lock lock_{nullptr};
};

} // namespace neko::detail
