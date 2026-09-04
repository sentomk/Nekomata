// log.hpp — nekomata's diagnostics: level cats, colored on a TTY.
//
//   (=･ω･=) or (ฅ´ω`ฅ)  info   alive, occasionally pawing  (cyan)
//   (=^ω^=)             ok     content — reload applied     (green)
//   (=¬ω¬=)             warn   side-eye — handled, grudgingly (yellow, reserved)
//   (=×ω×=)             error  playing dead — rejected      (red)
//
// The info face is picked per line from two candidates — deliberately via
// an UNSEEDED rand(): varied across lines, identical across runs, so
// transcripts stay reproducible.
//
// When stderr is not a TTY (pipes, CI, captured demos) the same cats are
// printed plain (no ANSI), keeping logs greppable.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace neko {

enum class log_level : std::uint8_t {
    info,
    ok,
    warn,
    error,
};

namespace detail {

inline bool stderr_is_tty() {
    static const bool tty = isatty(fileno(stderr)) != 0;
    return tty;
}

} // namespace detail

/// The level's cat, ANSI-colored on a TTY, plain otherwise.
inline const char* log_tag(log_level level) {
    const bool tty = detail::stderr_is_tty();
    switch (level) {
    case log_level::info: {
        static const char* const plain[] = {"(ฅ´ω`ฅ)", "(=･ω･=)"};
        static const char* const cyan[] = {"\033[36m(ฅ´ω`ฅ)\033[0m", "\033[36m(=･ω･=)\033[0m"};
        const int i = std::rand() % 2; // unseeded on purpose (see above)
        return tty ? cyan[i] : plain[i];
    }
    case log_level::ok:
        return tty ? "\033[32m(=^ω^=)\033[0m" : "(=^ω^=)";
    case log_level::warn: // reserved — Phase 5 layout-migration cases
        return tty ? "\033[33m(=¬ω¬=)\033[0m" : "(=¬ω¬=)";
    case log_level::error:
        return tty ? "\033[31m(=×ω×=)\033[0m" : "(=×ω×=)";
    }
    return "";
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
