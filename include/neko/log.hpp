// Public diagnostic API. Formatting and terminal handling live in the library.
#pragma once

#include <cstdint>

namespace neko {

enum class log_level : std::uint8_t {
  info,
  ok,
  warn,
  error,
};

/// The level's cat, ANSI-colored on a TTY, plain otherwise.
const char* log_tag(log_level level);

/// Log one diagnostic line to stderr: the cat, a space, then the message.
void log(log_level level, const char* fmt, ...);

} // namespace neko
