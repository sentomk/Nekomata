// log.cpp — nekomata's diagnostics: level cats, colored on a TTY.
//
//   (=•ω•=)  info   alert, curious                    (cyan)
//   (=^ω^=)  ok     content — reload applied          (green)
//   (=¬ω¬=)  warn   side-eye — handled, grudgingly    (yellow, reserved)
//   (=×ω×=)  error  playing dead — rejected           (red)
//
// Faces are defined in <neko/cats.hpp> — one source of truth for the
// logo animation and the log-level cats alike.
//
// When stderr is not a TTY (pipes, CI, captured demos) the same cats are
// printed plain (no ANSI), keeping logs greppable. Under Emscripten each line
// goes to the browser console method matching its level, plain: stderr there
// would print every line through console.error.

#include "cats.hpp"
#include <neko/log.hpp>

#include <cstdarg>
#include <cstdio>

#if defined(__EMSCRIPTEN__)
#include <emscripten/console.h>

#include <string>
#elif defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace neko {

namespace {

bool stderr_is_tty() {
#if defined(__EMSCRIPTEN__)
  return false;
#else
#if defined(_WIN32)
  static const bool tty = _isatty(_fileno(stderr)) != 0;
#else
  static const bool tty = isatty(fileno(stderr)) != 0;
#endif
  return tty;
#endif
}

const char* colored(const char* plain, const char* ansi_code) {
  static thread_local char buf[64];
  std::snprintf(buf, sizeof(buf), "\033[%sm%s\033[0m", ansi_code, plain);
  return buf;
}

} // namespace

/// The level's cat, ANSI-colored on a TTY, plain otherwise.
const char* log_tag(log_level level) {
  const bool tty = stderr_is_tty();
  switch (level) {
  case log_level::info:
    return tty ? colored(cats::info, "36") : cats::info;
  case log_level::ok:
    return tty ? colored(cats::ok, "32") : cats::ok;
  case log_level::warn:
    return tty ? colored(cats::warn, "33") : cats::warn;
  case log_level::error:
    return tty ? colored(cats::error, "31") : cats::error;
  }
  return "";
}

/// Log one diagnostic line to stderr: the cat, a space, then the message.
void log(log_level level, const char* fmt, ...) {
#if defined(__EMSCRIPTEN__)
  va_list args;
  va_start(args, fmt);
  va_list measure;
  va_copy(measure, args);
  const int length = std::vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  std::string line = std::string{log_tag(level)} + " ";
  if (length > 0) {
    const auto prefix = line.size();
    line.resize(prefix + static_cast<std::size_t>(length) + 1);
    std::vsnprintf(line.data() + prefix, static_cast<std::size_t>(length) + 1, fmt, args);
    line.resize(prefix + static_cast<std::size_t>(length));
  }
  va_end(args);
  // The console adds its own line break.
  while (!line.empty() && line.back() == '\n') {
    line.pop_back();
  }
  switch (level) {
  case log_level::info:
  case log_level::ok:
    emscripten_console_log(line.c_str());
    break;
  case log_level::warn:
    emscripten_console_warn(line.c_str());
    break;
  case log_level::error:
    emscripten_console_error(line.c_str());
    break;
  }
#else
  std::fprintf(stderr, "%s ", log_tag(level));
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
#endif
}

} // namespace neko
