// log.hpp — nekomata's diagnostics: level cats, colored on a TTY.
//
//   =^･ω･^= info    neutral, watching        (default)
//   =^ω^=   ok      content — reload applied (green)
//   =×ω×=   error   playing dead — rejected  (red)
//
// When stderr is not a TTY (pipes, CI, captured demos) the tags fall back
// to bracketed plain text so logs stay greppable and clean.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

namespace neko {

enum class log_level : std::uint8_t {
    info,
    ok,
    error,
    // warn — reserved for Phase 5 "layout drifted but migrated" style cases;
    // design its cat when the first real use appears.
};

namespace detail {

inline bool stderr_is_tty() {
    static const bool tty = isatty(fileno(stderr)) != 0;
    return tty;
}

} // namespace detail

/// The level's cat, ANSI-colored on a TTY, bracketed when plain.
inline const char* log_tag(log_level level) {
    if (!detail::stderr_is_tty()) {
        switch (level) {
        case log_level::ok:
            return "[=^ω^=]";
        case log_level::error:
            return "[=×ω×=]";
        case log_level::info:
            break;
        }
        return "[=^･ω･^=]";
    }
    switch (level) {
    case log_level::ok:
        return "\033[32m=^ω^=\033[0m";
    case log_level::error:
        return "\033[31m=×ω×=\033[0m";
    case log_level::info:
        break;
    }
    return "=^･ω･^=";
}

/// Log one diagnostic line to stderr: the cat, a space, then the message.
inline void log(log_level level, const char* fmt, ...) {
    std::fprintf(stderr, "%s ", log_tag(level));
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

} // namespace neko
