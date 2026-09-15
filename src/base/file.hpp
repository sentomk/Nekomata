#pragma once

#include "c/neko_file.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::detail {

/// Whole-file read as text. Returns nullopt when the path is not a readable
/// regular file; callers own the failure wording.
[[nodiscard]] std::optional<std::string> read_file_if_present(const std::filesystem::path& path);

/// Whole-file read; throws `<prefix><path>` on any failure. The prefix is
/// the caller's stable message, the path is appended verbatim.
[[nodiscard]] std::string read_required_file(const std::filesystem::path& path,
                                             std::string_view prefix);

/// Byte form of `read_required_file` for binary consumers.
[[nodiscard]] std::vector<std::uint8_t> read_required_bytes(const std::filesystem::path& path,
                                                            std::string_view prefix);

/// Truncating whole-file write; throws `<prefix> '<path>'` on failure.
/// Creating parent directories is caller policy.
void write_required_file(const std::filesystem::path& path, std::string_view bytes,
                         std::string_view prefix);

namespace base_detail {

/// UTF-8 view of a path for the C layer's `const char*` ABI.
[[nodiscard]] inline std::string utf8_path(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string encoded = path.u8string();
  return std::string{reinterpret_cast<const char*>(encoded.c_str()), encoded.size()};
#else
  return path.generic_string();
#endif
}

class contents_guard {
public:
  explicit contents_guard(neko_file_contents* contents) : contents_{contents} {}
  ~contents_guard() { neko_file_contents_free(contents_); }
  contents_guard(const contents_guard&) = delete;
  contents_guard& operator=(const contents_guard&) = delete;

private:
  neko_file_contents* contents_;
};

} // namespace base_detail

inline std::optional<std::string> read_file_if_present(const std::filesystem::path& path) {
  neko_file_contents contents;
  if (neko_file_read(base_detail::utf8_path(path).c_str(), &contents) != neko_file_ok) {
    return std::nullopt;
  }
  base_detail::contents_guard guard{&contents};
  return std::string{reinterpret_cast<const char*>(contents.data), contents.size};
}

inline std::string read_required_file(const std::filesystem::path& path, std::string_view prefix) {
  neko_file_contents contents;
  if (neko_file_read(base_detail::utf8_path(path).c_str(), &contents) != neko_file_ok) {
    throw std::runtime_error(std::string{prefix} + path.generic_string());
  }
  base_detail::contents_guard guard{&contents};
  return std::string{reinterpret_cast<const char*>(contents.data), contents.size};
}

inline std::vector<std::uint8_t> read_required_bytes(const std::filesystem::path& path,
                                                     std::string_view prefix) {
  const std::string bytes = read_required_file(path, prefix);
  return {bytes.begin(), bytes.end()};
}

inline void write_required_file(const std::filesystem::path& path, std::string_view bytes,
                                std::string_view prefix) {
  if (neko_file_write(base_detail::utf8_path(path).c_str(), bytes.data(), bytes.size()) !=
      neko_file_ok) {
    throw std::runtime_error(std::string{prefix} + " '" + path.generic_string() + "'");
  }
}

} // namespace neko::detail
